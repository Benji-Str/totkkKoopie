// Local co-op link, experimental (coop.h has the overview). Off unless coop = 1: then main() starts one link thread
// that sends this side's pose to coop_peer and takes the peer's, and the overlay asks coop_view for its markers.
//
// The camera half: the replay exports the scene camera at each present (vk_dlaa.c export_frame), coop_view projects
// the poses through it. coop_marker = 2 is the test mode for that path before a player hook exists: a point 10 m in
// front of the camera (worked out from the view matrix alone) must land in the middle of the picture through the
// view-projection, and is published as this side's pose, so with coop_peer = loop it comes back as the "peer" and its
// marker covers the test cross; between two machines each sees the point in front of the other's camera.
#include <atomic>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "cfg.h"
#include "coop.h"
#include "platform/platform.h"
// host.h pulls in cpu.h, which is C only
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

#define COOP_HZ 30
#define COOP_MAGIC 0x50434b54u   // "TKCP" little endian
#define COOP_VERSION 1u
#define COOP_TIMEOUT_NS 2000000000ull   // no packet from the peer this long: waiting again, and its count may restart
#define COOP_WORLD_LIMIT 100000.0f      // |coordinate| a pose may have; TotK's map spans about +-6000
#define COOP_TEST_DIST 10.0f            // metres in front of the camera, coop_marker = 2
#define CAMERA_MAX_AGE_NS 250000000ull  // an older camera is a loading screen or a paused replay: no markers
#define REMOTE_MAX_AGE_NS 500000000ull  // the peer sends 30 a second: half a second without one hides its marker

// ---- seqlock: one value of whole words, any number of writers (serialised by busy) and readers (retry while a write
// is under way). The words are relaxed atomics, so a reader racing a writer reads defined values and then discards them.
template <class T> struct Seq {
    static_assert(sizeof(T) % 4 == 0, "a seqlock value is whole words");
    static constexpr unsigned N = sizeof(T) / 4;
    std::atomic<uint32_t> gen{0};   // odd while a write is under way; 0: never written
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> w[N];

    void store(const T &v) {
        uint32_t src[N];
        memcpy(src, &v, sizeof src);
        while (busy.test_and_set(std::memory_order_acquire)) plat_pause();
        uint32_t g = gen.load(std::memory_order_relaxed);
        gen.store(g + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < N; i++) w[i].store(src[i], std::memory_order_relaxed);
        gen.store(g + 2, std::memory_order_release);
        busy.clear(std::memory_order_release);
    }

    bool load(T *out) const {
        uint32_t dst[N];
        for (int tries = 0; tries < 1000; tries++) {
            uint32_t g = gen.load(std::memory_order_acquire);
            if (!g) return false;
            if (g & 1) {
                plat_pause();
                continue;
            }
            for (unsigned i = 0; i < N; i++) dst[i] = w[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (gen.load(std::memory_order_relaxed) == g) {
                memcpy(out, dst, sizeof dst);
                return true;
            }
        }
        return false;   // a writer kept it busy for the whole time: the caller acts as if nothing was there
    }
};

struct CoopState {
    Seq<CoopPose> local, remote;
    Seq<CoopCamera> camera;
    std::atomic<int> on{0};            // the link thread runs
    std::atomic<int> cam_wanted{0};    // coop_view draws markers: the replay exports the camera
    std::atomic<uint32_t> local_seq{0};
    std::atomic<uint64_t> hook_ns{0};  // the last coop_publish_local: a real pose, the test point stays out
    std::atomic<uint64_t> tx{0}, rx{0}, dropped{0}, last_rx_ns{0};
};
static CoopState S;

// ---- poses

static void publish(CoopPose p) {
    p.seq = S.local_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    p.t_ns = plat_now_ns();
    S.local.store(p);
}

extern "C" void coop_publish_local(const CoopPose *p) {
    S.hook_ns.store(plat_now_ns(), std::memory_order_relaxed);
    publish(*p);
}

extern "C" int coop_read_local(CoopPose *out) { return S.local.load(out) && (out->status & COOP_VALID); }

extern "C" int coop_read_remote(CoopPose *out, uint64_t max_age_ns) {
    if (!S.remote.load(out) || !(out->status & COOP_VALID)) return 0;
    return out->t_ns + max_age_ns >= plat_now_ns();   // t_ns may be a hair after a reader's clock: no subtraction
}

extern "C" int coop_link_state(void) {
    if (!S.on.load(std::memory_order_relaxed)) return 0;
    uint64_t last = S.last_rx_ns.load(std::memory_order_relaxed);
    return last && last + COOP_TIMEOUT_NS >= plat_now_ns() ? 2 : 1;
}

// ---- the camera

extern "C" int coop_camera_wanted(void) { return S.cam_wanted.load(std::memory_order_relaxed); }

extern "C" void coop_camera_note(const float rows[28], uint32_t w, uint32_t h, uint64_t frame) {
    CoopCamera c;
    memcpy(c.view, rows, sizeof c.view);
    memcpy(c.vp, rows + 12, sizeof c.vp);
    c.frame = frame, c.t_ns = plat_now_ns(), c.w = w, c.h = h, c.pad = 0;
    S.camera.store(c);
}

extern "C" int coop_read_camera(CoopCamera *out, uint64_t max_age_ns) {
    return S.camera.load(out) && out->t_ns + max_age_ns >= plat_now_ns();
}

// The camera's position and its forward direction (unit length), from the view matrix alone. view = [R | t] maps the
// world to view space, so the camera sits at -R^T t; an OpenGL view looks down its -Z, which is -R's third row in the
// world.
static int camera_frame(const CoopCamera *c, double pos[3], double fwd[3]) {
    const float *v = c->view;
    for (int j = 0; j < 3; j++) pos[j] = -(v[0 * 4 + j] * (double)v[3] + v[1 * 4 + j] * (double)v[7] + v[2 * 4 + j] * (double)v[11]);
    double n = sqrt((double)v[8] * v[8] + (double)v[9] * v[9] + (double)v[10] * v[10]);
    if (!(n > 1e-9) || !isfinite(pos[0] + pos[1] + pos[2])) return 0;
    for (int j = 0; j < 3; j++) fwd[j] = -v[8 + j] / n;
    return 1;
}

// p through the view-projection into the picture rect (pixels {x, y, w, h}). 1: on the picture; 2: off it or behind
// the camera, xy then on the picture's border in the direction of the point; 0: no answer (w of 0, not finite).
// ndc gets the point's normalised device coordinates when it is in front of the camera.
static int project(const CoopCamera *c, const double p[3], const float rect[4], float xy[2], float ndc[2]) {
    double clip[4];
    for (int r = 0; r < 4; r++)
        clip[r] = c->vp[r * 4 + 0] * p[0] + c->vp[r * 4 + 1] * p[1] + c->vp[r * 4 + 2] * p[2] + c->vp[r * 4 + 3];
    double w = clip[3];
    if (!(fabs(w) > 1e-9) || !isfinite(clip[0] + clip[1])) return 0;
    double nx = clip[0] / w, ny = clip[1] / w;
    if (ndc && w > 0) ndc[0] = (float)nx, ndc[1] = (float)ny;
    int on = w > 0 && fabs(nx) <= 1 && fabs(ny) <= 1;
    if (!on) {
        if (w < 0) nx = -nx, ny = -ny;   // behind: x / w and y / w point away from it
        double m = fmax(fabs(nx), fabs(ny));
        if (m < 1e-9) nx = 0, ny = -1, m = 1;   // straight behind: the bottom edge
        nx /= m, ny /= m;
    }
    xy[0] = rect[0] + (float)((nx * 0.5 + 0.5) * rect[2]);
    xy[1] = rect[1] + (float)((1.0 - ny) * 0.5 * rect[3]);
    return on ? 1 : 2;
}

static int project_pose(const CoopCamera *c, const CoopPose *p, const float rect[4], float xy[2]) {
    double q[3] = {p->pos[0], p->pos[1], p->pos[2]};
    return project(c, q, rect, xy, NULL);
}

static int marker_setting(void) {
    int m = cfg_int("TOTK_COOP_MARKER");
    return m < 0 ? 0 : m > 2 ? 2 : m;
}

extern "C" int coop_overlay_on(void) {
    int on = S.on.load(std::memory_order_relaxed) && marker_setting() > 0;
    S.cam_wanted.store(on, std::memory_order_relaxed);   // coop_marker = 0 while running stops the camera export too
    return on;
}

extern "C" void coop_view(const float rect[4], CoopView *v) {
    memset(v, 0, sizeof *v);
    v->marker = S.on.load(std::memory_order_relaxed) ? marker_setting() : 0;
    S.cam_wanted.store(v->marker > 0, std::memory_order_relaxed);
    if (!v->marker) return;
    v->link = coop_link_state();
    v->tx = S.tx.load(std::memory_order_relaxed);
    v->rx = S.rx.load(std::memory_order_relaxed);
    v->dropped = S.dropped.load(std::memory_order_relaxed);
    CoopCamera c;
    double cpos[3], fwd[3];
    v->cam_ok = coop_read_camera(&c, CAMERA_MAX_AGE_NS) && camera_frame(&c, cpos, fwd);
    uint64_t now = plat_now_ns();
    if (v->cam_ok && v->marker == 2) {
        double t[3];
        for (int j = 0; j < 3; j++) t[j] = cpos[j] + fwd[j] * COOP_TEST_DIST;
        v->check_on = project(&c, t, rect, v->check_xy, v->check_ndc);
        // the test point as this side's pose while no player hook publishes one
        if (S.hook_ns.load(std::memory_order_relaxed) + 1000000000ull < now) {
            CoopPose p = {};
            for (int j = 0; j < 3; j++) p.pos[j] = (float)t[j];
            p.yaw = (float)atan2(fwd[0], fwd[2]);
            p.status = COOP_VALID | COOP_TEST;
            publish(p);
        }
        static uint64_t calls;
        if (++calls % 600 == 1)
            rt_log("coop: test: camera of frame %llu (%ux%u), test point at ndc (%.4f, %.4f), expected (0, 0); link %d, "
                   "%llu sent, %llu received, %llu refused", (unsigned long long)c.frame, c.w, c.h, v->check_ndc[0],
                   v->check_ndc[1], v->link, (unsigned long long)v->tx, (unsigned long long)v->rx,
                   (unsigned long long)v->dropped);
    }
    if (!v->cam_ok) return;
    CoopPose r;
    if (coop_read_remote(&r, REMOTE_MAX_AGE_NS)) {
        v->remote_on = project_pose(&c, &r, rect, v->remote_xy);
        double d[3] = {r.pos[0] - cpos[0], r.pos[1] - cpos[1], r.pos[2] - cpos[2]};
        v->remote_dist = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        v->remote_test = (r.status & COOP_TEST) != 0;
    }
    CoopPose l;
    if (v->marker == 2 && coop_read_local(&l) && !(l.status & COOP_TEST)) v->local_on = project_pose(&c, &l, rect, v->local_xy);
}

// ---- the wire format

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

extern "C" void coop_pack(const CoopPose *p, uint8_t out[COOP_PACKET_BYTES]) {
    put32(out, COOP_MAGIC);
    out[4] = COOP_VERSION, out[5] = out[6] = out[7] = 0;
    put32(out + 8, p->seq);
    for (int j = 0; j < 3; j++) put32(out + 12 + 4 * j, fbits(p->pos[j]));
    put32(out + 24, fbits(p->yaw));
    put32(out + 28, p->status);
}

extern "C" int coop_unpack(const uint8_t *in, size_t n, CoopPose *out) {
    if (n != COOP_PACKET_BYTES || get32(in) != COOP_MAGIC || in[4] != COOP_VERSION || in[5] || in[6] || in[7]) return 0;
    CoopPose p = {};
    p.seq = get32(in + 8);
    for (int j = 0; j < 3; j++) {
        p.pos[j] = bitsf(get32(in + 12 + 4 * j));
        if (!isfinite(p.pos[j]) || fabsf(p.pos[j]) > COOP_WORLD_LIMIT) return 0;
    }
    p.yaw = bitsf(get32(in + 24));
    p.status = get32(in + 28);
    if (!isfinite(p.yaw) || fabsf(p.yaw) > 16.0f || (p.status & ~COOP_STATUS_KNOWN)) return 0;
    *out = p;
    return 1;
}

static const char *parse_num(const char *s, uint32_t max, uint32_t *out) {
    uint32_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9' && digits < 6; s++, digits++) v = v * 10 + (uint32_t)(*s - '0');
    if (!digits || digits > 5 || v > max) return NULL;
    *out = v;
    return s;
}

extern "C" int coop_parse_addr(const char *s, uint16_t default_port, PlatUdpAddr *out) {
    if (!s) return 0;
    uint32_t ip = 0, part, port = default_port;
    for (int i = 0; i < 4; i++) {
        if (!(s = parse_num(s, 255, &part))) return 0;
        ip = ip << 8 | part;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s == ':' && (!(s = parse_num(s + 1, 65535, &port)) || !port)) return 0;
    if (*s || !ip) return 0;
    out->ip = ip, out->port = (uint16_t)port;
    return 1;
}

// ---- the link thread

struct Link {
    PlatUdp *u;
    PlatUdpAddr peer;
    std::atomic<int> *stop;   // NULL: runs until the process ends
};

// Sends this side's pose (or a keep-alive without COOP_VALID when none is recent) COOP_HZ times a second and waits for
// the peer's packets in between. Only packets from the peer's address and port count, and only newer ones than the
// last: after COOP_TIMEOUT_NS of silence the peer may have restarted, and its count with it.
static void link_run(const Link *k) {
    const uint64_t period = 1000000000ull / COOP_HZ;
    uint64_t next = plat_now_ns(), last_ok = 0;
    uint32_t tx_seq = 0, last_seq = 0;
    while (!k->stop || !k->stop->load(std::memory_order_relaxed)) {
        uint64_t now = plat_now_ns();
        if (now >= next) {
            CoopPose p;
            if (!S.local.load(&p) || p.t_ns + COOP_TIMEOUT_NS < now) memset(&p, 0, sizeof p);
            p.seq = ++tx_seq;   // the wire count is the link's own: the receiver orders packets by it
            uint8_t pkt[COOP_PACKET_BYTES];
            coop_pack(&p, pkt);
            if (plat_udp_send(k->u, &k->peer, pkt, sizeof pkt)) S.tx.fetch_add(1, std::memory_order_relaxed);
            next += period;
            if (next <= now) next = now + period;   // after a stall: no burst to catch up
        }
        uint8_t buf[64];
        PlatUdpAddr from;
        uint64_t left = next > now ? next - now : 0;
        int n = plat_udp_recv(k->u, buf, sizeof buf, &from, (uint32_t)(left / 1000000u) + 1);
        if (n < 0) {
            rt_log("coop: the UDP socket failed; the link stops");
            S.on.store(0, std::memory_order_relaxed);
            return;
        }
        if (!n) continue;
        CoopPose r;
        if (from.ip != k->peer.ip || from.port != k->peer.port || !coop_unpack(buf, (size_t)n, &r)) {
            S.dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        now = plat_now_ns();
        if (last_ok && last_ok + COOP_TIMEOUT_NS >= now && (int32_t)(r.seq - last_seq) <= 0) {
            S.dropped.fetch_add(1, std::memory_order_relaxed);   // late or repeated
            continue;
        }
        last_seq = r.seq, last_ok = now;
        r.t_ns = now;
        S.remote.store(r);   // a keep-alive too: its missing COOP_VALID hides the marker
        S.rx.fetch_add(1, std::memory_order_relaxed);
        S.last_rx_ns.store(now, std::memory_order_relaxed);
    }
}

static void link_main(void *arg) { link_run((const Link *)arg); }

extern "C" void coop_init(void) {
    if (!cfg_bool("TOTK_COOP")) return;
    int port = cfg_int("TOTK_COOP_PORT");
    if (port < 1024 || port > 65535) {
        rt_log("coop: coop_port %d is not in 1024..65535: co-op stays off", port);
        return;
    }
    static Link k;
    const char *peer = cfg_get("TOTK_COOP_PEER");
    int loop = peer && !strcmp(peer, "loop");
    if (!loop && !coop_parse_addr(peer, (uint16_t)port, &k.peer)) {
        rt_log("coop: coop_peer '%s' is not a.b.c.d, a.b.c.d:port or loop: co-op stays off", peer ? peer : "");
        return;
    }
    if (loop) k.peer = PlatUdpAddr{0x7f000001u, (uint16_t)port};
    uint32_t err = 0;
    k.u = plat_udp_open(0, (uint16_t)port, &err);
    if (!k.u) {
        rt_log("coop: cannot open UDP port %d (error %u): co-op stays off", port, err);
        return;
    }
    S.on.store(1, std::memory_order_relaxed);
    S.cam_wanted.store(marker_setting() > 0, std::memory_order_relaxed);
    if (!plat_thread_spawn(link_main, &k, "coop", 0)) {
        rt_log("coop: cannot start the link thread: co-op stays off");
        S.on.store(0, std::memory_order_relaxed);
        S.cam_wanted.store(0, std::memory_order_relaxed);
        plat_udp_close(k.u);
        return;
    }
    rt_log("coop: link on UDP port %d to %u.%u.%u.%u:%u, %d packets a second (experimental)", port,
           k.peer.ip >> 24, k.peer.ip >> 16 & 255, k.peer.ip >> 8 & 255, k.peer.ip & 255, k.peer.port, COOP_HZ);
}

// ---- --probe-coop: no game files, no window

// A camera at pos turned yaw radians about +Y (yaw 0 looks down -Z), OpenGL perspective of the given vertical field
// of view; rows as the scene block holds them (view 3x4, then view-projection 4x4).
static void make_camera(CoopCamera *c, const double pos[3], double yaw, double fovy, double aspect) {
    double cy = cos(yaw), sy = sin(yaw);
    // the view's rotation rows are the camera's right (cy, 0, -sy), up (0, 1, 0) and back (sy, 0, cy) in the world
    double R[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
    double V[4][4] = {};
    for (int i = 0; i < 3; i++) {
        double t = 0;
        for (int j = 0; j < 3; j++) V[i][j] = R[i][j], t -= R[i][j] * pos[j];
        V[i][3] = t;
    }
    V[3][3] = 1;
    double n = 0.1, f = 1000, k = 1 / tan(fovy / 2);
    double P[4][4] = {{k / aspect, 0, 0, 0}, {0, k, 0, 0}, {0, 0, (f + n) / (n - f), 2 * f * n / (n - f)}, {0, 0, -1, 0}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) c->view[i * 4 + j] = (float)V[i][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int m = 0; m < 4; m++) s += P[i][m] * V[m][j];
            c->vp[i * 4 + j] = (float)s;
        }
    c->frame = 1, c->t_ns = plat_now_ns(), c->w = 1920, c->h = 1080, c->pad = 0;
}

static std::atomic<int> stress_stop;

static void stress_writer(void *arg) {
    (void)arg;
    for (uint32_t k = 1; !stress_stop.load(std::memory_order_relaxed); k++) {
        CoopPose p = {{(float)k, (float)k, (float)k}, (float)k, COOP_VALID, k, k};
        S.remote.store(p);
    }
}

extern "C" int probe_coop(void) {
    int fails = 0;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop: %s\n", what); } } while (0)
    // packets
    CoopPose a = {{12.5f, -300.25f, 4096.0f}, 1.25f, COOP_VALID, 77, 0}, b;
    uint8_t pkt[COOP_PACKET_BYTES];
    coop_pack(&a, pkt);
    CHECK(coop_unpack(pkt, sizeof pkt, &b) && !memcmp(b.pos, a.pos, sizeof a.pos) && b.yaw == a.yaw &&
              b.status == a.status && b.seq == a.seq, "a packet unpacks to what was packed");
    CHECK(pkt[0] == 'T' && pkt[1] == 'K' && pkt[2] == 'C' && pkt[3] == 'P', "the magic reads TKCP");
    CHECK(!coop_unpack(pkt, sizeof pkt - 1, &b), "a short packet is refused");
    uint8_t bad[COOP_PACKET_BYTES];
    memcpy(bad, pkt, sizeof bad), bad[0] ^= 1;
    CHECK(!coop_unpack(bad, sizeof bad, &b), "a wrong magic is refused");
    memcpy(bad, pkt, sizeof bad), bad[4] = 2;
    CHECK(!coop_unpack(bad, sizeof bad, &b), "another version is refused");
    CoopPose c = a;
    c.pos[1] = NAN;
    coop_pack(&c, bad);
    CHECK(!coop_unpack(bad, sizeof bad, &b), "a NaN coordinate is refused");
    c = a, c.pos[2] = 1e9f;
    coop_pack(&c, bad);
    CHECK(!coop_unpack(bad, sizeof bad, &b), "a coordinate outside the world is refused");
    c = a, c.yaw = INFINITY;
    coop_pack(&c, bad);
    CHECK(!coop_unpack(bad, sizeof bad, &b), "an infinite yaw is refused");
    c = a, c.status = 0x80;
    coop_pack(&c, bad);
    CHECK(!coop_unpack(bad, sizeof bad, &b), "unknown status bits are refused");

    // addresses
    PlatUdpAddr ad;
    CHECK(coop_parse_addr("192.168.1.20", 27460, &ad) && ad.ip == 0xc0a80114u && ad.port == 27460,
          "a.b.c.d takes the default port");
    CHECK(coop_parse_addr("10.0.0.2:4000", 27460, &ad) && ad.ip == 0x0a000002u && ad.port == 4000, "a.b.c.d:port");
    static const char *const wrong[] = {"", "abc", "1.2.3", "256.1.1.1", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:70000",
                                        "1.2.3.4 ", "0.0.0.0", "1.2.3.4.5", "01234.1.1.1", "localhost"};
    for (const char *s : wrong) {
        char what[64];
        snprintf(what, sizeof what, "'%s' is refused", s);
        CHECK(!coop_parse_addr(s, 27460, &ad), what);
    }
    CHECK(!coop_parse_addr(NULL, 27460, &ad), "no address is refused");

    // projection: a 16:9 picture at (100, 50) in the swapchain, cameras looking several ways
    const float rect[4] = {100, 50, 1600, 900};
    const double cams[][4] = {{100, 50, 200, 0}, {-3000, 120, 2500, 1.5707963}, {0, 0, 0, 3.0}, {5000, 400, -4000, -2.2}};
    for (const auto &cm : cams) {
        CoopCamera cam;
        make_camera(&cam, cm, cm[3], 60 * 3.14159265358979 / 180, 16.0 / 9);
        double pos[3], fwd[3], t[3];
        CHECK(camera_frame(&cam, pos, fwd), "the camera's position comes out of the view");
        CHECK(fabs(pos[0] - cm[0]) < 0.05 && fabs(pos[1] - cm[1]) < 0.05 && fabs(pos[2] - cm[2]) < 0.05,
              "the camera sits where it was put");
        for (int j = 0; j < 3; j++) t[j] = pos[j] + fwd[j] * COOP_TEST_DIST;
        float xy[2], ndc[2] = {9, 9};
        CHECK(project(&cam, t, rect, xy, ndc) == 1 && fabsf(xy[0] - 900) < 0.5f && fabsf(xy[1] - 500) < 0.5f &&
                  fabsf(ndc[0]) < 1e-3f && fabsf(ndc[1]) < 1e-3f, "the test point lands in the middle of the picture");
        double right[3] = {cos(cm[3]), 0, -sin(cm[3])};
        double q[3];
        for (int j = 0; j < 3; j++) q[j] = t[j] + right[j] * 2;
        CHECK(project(&cam, q, rect, xy, NULL) == 1 && xy[0] > 900.5f && fabsf(xy[1] - 500) < 0.5f,
              "a point to the right lands right of the middle");
        for (int j = 0; j < 3; j++) q[j] = t[j];
        q[1] += 2;
        CHECK(project(&cam, q, rect, xy, NULL) == 1 && xy[1] < 499.5f, "a point above lands higher (y down)");
        for (int j = 0; j < 3; j++) q[j] = pos[j] - fwd[j] * 10 + right[j] * 3;
        int r = project(&cam, q, rect, xy, NULL);
        CHECK(r == 2 && (fabsf(xy[0] - 100) < 0.5f || fabsf(xy[0] - 1700) < 0.5f || fabsf(xy[1] - 50) < 0.5f ||
                         fabsf(xy[1] - 950) < 0.5f), "a point behind the camera goes to the picture's border");
        CHECK(r == 2 && xy[0] > 900, "a point behind and to the right points right");
        for (int j = 0; j < 3; j++) q[j] = t[j] + right[j] * 1000;
        CHECK(project(&cam, q, rect, xy, NULL) == 2 && fabsf(xy[0] - 1700) < 0.5f, "a point far right goes to the right border");
    }

    // seqlock: a reader never sees a pose half written
    stress_stop.store(0);
    PlatThread *wt = plat_thread_new(stress_writer, NULL, "coop-stress");
    CHECK(wt != NULL, "the stress writer starts");
    uint64_t reads = 0, torn = 0, end = plat_now_ns() + 300000000ull;
    while (wt && plat_now_ns() < end) {
        CoopPose p;
        if (!S.remote.load(&p)) continue;
        reads++;
        torn += !(p.pos[0] == p.pos[1] && p.pos[1] == p.pos[2] && p.yaw == p.pos[0] && (float)p.seq == p.pos[0] &&
                  p.t_ns == p.seq);
    }
    stress_stop.store(1);
    if (wt) plat_thread_join(wt);
    fprintf(stderr, "coop: seqlock: %llu reads during writes, %llu torn\n", (unsigned long long)reads,
            (unsigned long long)torn);
    CHECK(reads > 1000 && !torn, "seqlock reads are whole");

    // UDP over loopback
    uint32_t err = 0;
    PlatUdp *u1 = plat_udp_open(0x7f000001u, 0, &err), *u2 = plat_udp_open(0x7f000001u, 0, &err);
    CHECK(u1 && u2, "two UDP sockets open on 127.0.0.1");
    if (u1 && u2) {
        PlatUdpAddr to = {0x7f000001u, plat_udp_port(u2)}, from = {};
        CHECK(to.port && plat_udp_port(u1), "the sockets know their ports");
        uint8_t got[64];
        CHECK(plat_udp_recv(u2, got, sizeof got, &from, 0) == 0, "an empty socket has nothing at once");
        uint64_t t0 = plat_now_ns();
        CHECK(plat_udp_recv(u2, got, sizeof got, &from, 50) == 0 && plat_now_ns() - t0 >= 40000000ull,
              "a receive waits for its time-out");
        CHECK(plat_udp_send(u1, &to, pkt, sizeof pkt), "a datagram goes out");
        int n = plat_udp_recv(u2, got, sizeof got, &from, 1000);
        CHECK(n == (int)sizeof pkt && !memcmp(got, pkt, sizeof pkt), "the datagram arrives whole");
        CHECK(from.ip == 0x7f000001u && from.port == plat_udp_port(u1), "the sender is known");
        CHECK(plat_udp_send(u1, &to, pkt, sizeof pkt) && plat_udp_recv(u2, got, 8, &from, 1000) == 8,
              "a datagram longer than the buffer is cut to it");
    }
    plat_udp_close(u1);
    plat_udp_close(u2);

    // the link: a socket that is its own peer, as coop_peer = loop
    S.remote.store(CoopPose{});
    S.local_seq.store(0), S.rx.store(0), S.tx.store(0), S.dropped.store(0), S.last_rx_ns.store(0);
    std::atomic<int> stop{0};
    Link k = {plat_udp_open(0x7f000001u, 0, &err), {}, &stop};
    CHECK(k.u != NULL, "the link's socket opens");
    if (k.u) {
        k.peer = PlatUdpAddr{0x7f000001u, plat_udp_port(k.u)};
        S.on.store(1);
        CoopPose me = {{-1234.5f, 88.0f, 765.25f}, -0.5f, COOP_VALID, 0, 0}, peer = {};
        coop_publish_local(&me);
        PlatThread *lt = plat_thread_new(link_main, &k, "coop-probe");
        int seen = 0;
        for (uint64_t until = plat_now_ns() + 2000000000ull; lt && !seen && plat_now_ns() < until;) {
            seen = coop_read_remote(&peer, REMOTE_MAX_AGE_NS);
            if (!seen) plat_sleep_ms(5);
        }
        CHECK(seen && !memcmp(peer.pos, me.pos, sizeof me.pos) && peer.yaw == me.yaw, "the pose comes back over the link");
        CHECK(coop_link_state() == 2, "the link counts the peer as heard");
        // an old packet from the peer's own address is refused, and so is a packet from another sender
        CoopPose old = me;
        old.seq = 1;
        coop_pack(&old, pkt);
        uint64_t dropped = S.dropped.load();
        plat_udp_send(k.u, &k.peer, pkt, sizeof pkt);
        for (uint64_t until = plat_now_ns() + 1000000000ull; S.dropped.load() == dropped && plat_now_ns() < until;)
            plat_sleep_ms(5);
        CHECK(S.dropped.load() > dropped, "an old packet from the peer is refused");
        // a well-formed, newer packet from another port must never become the peer's pose
        PlatUdp *o = plat_udp_open(0x7f000001u, 0, &err);
        CoopPose spoof = {{4242.0f, 4242.0f, 4242.0f}, 0, COOP_VALID, 0, 0};
        spoof.seq = 0x40000000u;   // newer than anything the link sent: only the sender is wrong
        coop_pack(&spoof, pkt);
        if (o) plat_udp_send(o, &k.peer, pkt, sizeof pkt);
        int spoofed = 0;
        for (uint64_t until = plat_now_ns() + 300000000ull; plat_now_ns() < until;) {
            CoopPose r;
            if (S.remote.load(&r) && r.pos[0] == 4242.0f) spoofed = 1;
            plat_sleep_ms(1);
        }
        CHECK(o && !spoofed, "a packet from another sender is refused");
        CHECK(coop_read_remote(&peer, REMOTE_MAX_AGE_NS) && peer.pos[0] == me.pos[0], "the peer's pose keeps coming");
        plat_udp_close(o);
        stop.store(1);
        if (lt) plat_thread_join(lt);
        fprintf(stderr, "coop: link: %llu sent, %llu received, %llu refused\n", (unsigned long long)S.tx.load(),
                (unsigned long long)S.rx.load(), (unsigned long long)S.dropped.load());
        S.on.store(0);
        plat_udp_close(k.u);
    }
    fprintf(stderr, "coop: probe %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
#undef CHECK
}
