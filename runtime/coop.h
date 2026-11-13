// Local co-op link (coop.cpp), experimental and off by default (coop = 1 in totk.ini).
//
// A host-side bridge between two copies of the game on one network: each side publishes its player's pose (position,
// yaw, status), a link thread sends it to the peer over UDP 30 times a second and stores what the peer sends, and the
// overlay (ui/menu.cpp) draws the peer's position as a marker over the picture, projected through the scene camera the
// replay exports (vk_dlaa.c). Nothing received is ever written into guest memory: a packet can at most move a marker.
//
// Who writes what (each value is a seqlock: readers never block a writer, and never see half a pose):
//   local    the game side, coop_publish_local: the player actor's position at each present (frame_rate.c
//            coop_player). While there is no player actor, coop_marker = 2 publishes the camera test point instead,
//            see coop_view
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

// ---- the pose of the player's skeletons, for the peer's actor (coop_puppet.c): per bone its local translation and
// rotation, the bone's flag word and seven values in units of 1/4096 (so within +-8), the 63 bones of the body and then the 35 of the face,
// and after them, in the same units, the nine numbers of the rotation the bones hang on (the model's, in rows) and
// the three of its offset from the pose's position.
// The link sends the last published set after each pose packet while it is younger than 200 ms, as one datagram of
// COOP_BONES_PACKET_BYTES: 0 magic "TKCB", 4 version, 5-7 zero, 8 seq, 12 the values, little endian. Every number of a
// well-formed packet is in range; the flag words are the peer's as they are (coop_puppet.c pose_apply).
#define COOP_BONES 98
#define COOP_BONE_FIELDS 9   // the flag word's two halves, three of translation, four of rotation
#define COOP_BONE_VALUES (COOP_BONES * COOP_BONE_FIELDS + 12)
#define COOP_BONES_PACKET_BYTES (12u + COOP_BONE_VALUES * 2u)
void coop_publish_bones(const int16_t v[COOP_BONE_VALUES]);
int coop_read_remote_bones(int16_t out[COOP_BONE_VALUES], uint64_t max_age_ns);   // 1 when the peer's set is recent

// ---- what the player wears: per slot of the equipment component the last piece the game put on through the player's
// piece function (coop_puppet.c host_piece_note): its name and the call's other three arguments. The link sends the
// last published table once a second and at once after a change, as one datagram: 0 magic "TKCE", 4 version, 5-7
// zero, 8 gen, 12 the count of entries, then per entry the slot, the flags, w3 (4 bytes), the name's length and the
// name (letters, digits and '_' only).
#define COOP_EQUIP_SLOTS 24
#define COOP_EQUIP_NAME 48           // with the terminating zero
#define COOP_EQUIP_NOTED 0x80u       // flags: the slot has an entry
#define COOP_EQUIP_W4 1u             // flags: the call's w4 and w5
#define COOP_EQUIP_W5 2u
#define COOP_EQUIP_WEAPON 4u         // flags: noted from the weapon function; w3 holds its x3 | x4 << 8 | x5 << 24
#define COOP_EQUIP_PACKET_MAX (13u + COOP_EQUIP_SLOTS * (7u + COOP_EQUIP_NAME - 1u))
typedef struct {
    char name[COOP_EQUIP_SLOTS][COOP_EQUIP_NAME];
    uint32_t w3[COOP_EQUIP_SLOTS];
    uint8_t flags[COOP_EQUIP_SLOTS];
    uint32_t gen;    // a new value with every change: the receiver dresses the puppet again when it differs
    uint32_t pad;
    uint64_t t_ns;
} CoopEquip;
void coop_publish_equip(const CoopEquip *e);
int coop_read_remote_equip(CoopEquip *out);   // 1 once the peer sent a table
size_t coop_pack_equip(const CoopEquip *e, uint8_t out[COOP_EQUIP_PACKET_MAX]);
int coop_unpack_equip(const uint8_t *in, size_t n, CoopEquip *out);   // 1 when well formed

// ---- the peer as an actor in the world (coop_puppet.c), coop_puppet = 1, 1.4.3 only. At each present, on the
// presenting guest thread; self: this side's player position; player: its actor, 0 when not known.
void coop_puppet_present(const float self[3], uint64_t player);

// ---- a session started from the start menu (ui/menu.cpp) instead of totk.ini. One link per run: a second start
// fails. A host opens coop_port and takes the sender of the first well-formed pose packet as its peer; a guest sends
// to the host's address. Either way the peer is shown as an actor (coop_puppet.c) without coop_puppet being set.
#define COOP_SESSION_NONE 0
#define COOP_SESSION_HOST 1
#define COOP_SESSION_JOIN 2
int coop_host(void);                  // 1 when the link runs
int coop_join(const char *addr);      // "a.b.c.d" or "a.b.c.d:port": 1 when the link runs
int coop_session(void);               // COOP_SESSION_*
int coop_peer_text(char out[32]);     // the peer's address, 1 once it is known
int coop_local_ip(char out[32]);      // this machine's address on its default route, 0 when it has none
void coop_trace_present(void);   // coop_puppet.c, the development trace TOTK_COOP_TRACE

int probe_coop(void);   // --probe-coop: packets, addresses, projection, seqlock, UDP and the link over loopback

#ifdef __cplusplus
}
#endif
