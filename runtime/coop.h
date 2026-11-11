// Local co-op link (coop.cpp), experimental and off by default (coop = 1 in totk.ini).
//
// A host-side bridge between two copies of the game on one network: each side publishes its player's pose (position,
// yaw, status), a link thread sends it to the peer over UDP 30 times a second and stores what the peer sends, and the
// overlay (ui/menu.cpp) draws the peer's position as a marker over the picture, projected through the scene camera the
// replay exports (vk_dlaa.c). Nothing received is ever written into guest memory: a packet can at most move a marker.
//
// Who writes what (each value is a seqlock: readers never block a writer, and never see half a pose):
//   local    the game side, coop_publish_local (the player hook still to come; until then coop_marker = 2 publishes the
//            camera test point instead, see coop_view)
//   remote   the link thread, from the peer's packets
//   camera   the replay, coop_camera_note at each present (vk_dlaa.c export_frame)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// CoopPose.status bits
#define COOP_VALID 1u   // pos and yaw hold a pose; without it the packet only keeps the link alive
#define COOP_TEST 2u    // the pose is coop_marker = 2's camera test point, not a player
#define COOP_STATUS_KNOWN (COOP_VALID | COOP_TEST)

typedef struct {
    float pos[3];     // world position, game units (metres)
    float yaw;        // radians about the world's up axis
    uint32_t status;  // COOP_* bits
    uint32_t seq;     // local: count of publishes; remote: the peer's packet count
    uint64_t t_ns;    // plat_now_ns when it was published (local) or received (remote)
} CoopPose;

// The scene camera as the game's scene uniform block holds it (constant bank c4 of the scene draws, see vk_dlaa.c and
// shaders/dlaa_mv.comp): rows of row-major matrices, clip = vp * (x, y, z, 1) with OpenGL clip depth, and window y
// growing downwards from ndc y = +1.
typedef struct {
    float view[12];   // vec4 rows 0-2 of the block: world to view, 3x4
    float vp[16];     // vec4 rows 3-6: view-projection, 4x4
    uint64_t frame;   // gfx_counters.frames at the export
    uint64_t t_ns;
    uint32_t w, h;    // the scene depth target it was drawn with
    uint32_t pad;
} CoopCamera;

// ---- life cycle and settings
void coop_init(void);        // main(), game runs only: starts the link thread when coop = 1 (read once)
int coop_link_state(void);   // 0 off, 1 waiting for the peer, 2 a packet from the peer within the last 2 s
int coop_overlay_on(void);   // the link is on and coop_marker > 0: the overlay has markers to lay out

// ---- poses, any thread
void coop_publish_local(const CoopPose *p);   // player 1; seq and t_ns are set here
int coop_read_local(CoopPose *out);           // 1 when a pose with COOP_VALID was published
int coop_read_remote(CoopPose *out, uint64_t max_age_ns);   // 1 when the peer's last pose is valid and recent enough

// ---- the scene camera, from the replay (vk_dlaa.c)
int coop_camera_wanted(void);   // one relaxed load: 0 unless markers are being drawn
// rows: vec4 rows 0-6 of the scene block (28 floats); w, h: its depth target; frame: gfx_counters.frames
void coop_camera_note(const float rows[28], uint32_t w, uint32_t h, uint64_t frame);
int coop_read_camera(CoopCamera *out, uint64_t max_age_ns);

// ---- the overlay (ui/menu.cpp), on the present path once a frame
typedef struct {
    int marker;               // coop_marker in effect; 0: draw nothing
    int link;                 // coop_link_state
    uint64_t tx, rx, dropped; // packets sent, accepted, refused (wrong sender, malformed, out of order)
    int cam_ok;               // a scene camera of the last 250 ms
    // markers: on 0 none, 1 on the picture, 2 off it or behind the camera (xy is then on the picture's border, towards it)
    int remote_on, local_on, check_on;
    float remote_xy[2], local_xy[2], check_xy[2];   // swapchain pixels
    float remote_dist;        // metres from the camera
    int remote_test;          // the peer sends its camera test point (its coop_marker = 2)
    float check_ndc[2];       // coop_marker = 2: the test point's ndc, (0, 0) when the camera export is read right
} CoopView;
// rect: where the picture lies in the swapchain image, pixels {x, y, w, h} (gfx_picture_rect scaled).
void coop_view(const float rect[4], CoopView *v);

// ---- the wire format: 32 bytes, little endian. 0 magic "TKCP", 4 version, 5-7 zero, 8 seq, 12 x, 16 y, 20 z, 24 yaw,
// 28 status. Exposed for the probe.
#define COOP_PACKET_BYTES 32u
void coop_pack(const CoopPose *p, uint8_t out[COOP_PACKET_BYTES]);
// 1 when in is a well-formed packet: right size, magic and version, finite numbers within the world, known status bits
int coop_unpack(const uint8_t *in, size_t n, CoopPose *out);
// "a.b.c.d" or "a.b.c.d:port", numbers only: 1 when valid; without ":port", out->port = default_port
int coop_parse_addr(const char *s, uint16_t default_port, PlatUdpAddr *out);

int probe_coop(void);   // --probe-coop: packets, addresses, projection, seqlock, UDP and the link over loopback

#ifdef __cplusplus
}
#endif
