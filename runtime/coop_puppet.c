// The co-op peer as an actor in the world, experimental and off by default (coop_puppet = 1 with coop = 1; game 1.4.3
// only). Where coop.cpp only draws a marker over the picture, this asks the game to create one actor and puts it at
// the peer's pose each present, so the peer is a figure in the scene. This is the one part of the co-op link that
// writes into the game: it calls two of the game's own functions on the thread that presents.
//
// The two functions, both placed in 1.4.3 from their 1.0.0 addresses in aquacluck/totk-lotuskit
// (syms/exking/engine/actor/ActorMgr.py, ActorBase.py) and checked by their first four instruction words before any
// call (a build whose words differ gets no puppet):
//   engine::actor::ActorMgr::requestCreateActorAsync   main+0x599670 (1.0.0 main+0xa5fdf8). Placed by its callers: 24
//       functions of 1.0.0 that call it share a string with one function of 1.4.3 each, and all 24 of those call
//       main+0x599670, which has 108 callers (1.0.0: 105). The game's own caller main+0x20c2114 creates
//       "Dm_Npc_RevivalFairy_ForPouchPlayer" with it, instruction for instruction as 1.0.0 main+0x1a2f144 does; the
//       arguments here follow that call: x0 the ActorMgr ([main+0x3ad7450], through the GOT slot main+0x395e008),
//       x1 a SafeString (one pointer to the name), x2 a CreateArg of 0x98 bytes, x3 a watcher reference, x4 the
//       priority, x5 a PreActor, x6 an actor file, x7 a callback, and on the stack a flag byte, a result word's
//       address and the address the request's PreActor is written to. As lotuskit's createAndWatchPosRot does, the
//       watcher, the file and the callback are null and the priority is 1 (high).
//   engine::actor::ActorBase::forceSetMatrix           main+0x45ce70 (MacNativeGames game_version.h TOTK_GAME_V143):
//       (actor, a 3x4 row-major matrix with the position in its fourth column, 0).
//
// The request is asynchronous: the game creates the actor on its own thread some frames later and stores it in the
// PreActor. That field's offset is not known, so the PreActor's first 0x200 bytes are searched each present for a
// pointer to an actor whose name (actor +0x218, frame_rate.c player_pos) is the requested one. The actor's name is
// read again before every move (plat_mem_read, no fault): once it no longer reads back (a load deleted the actor), the
// puppet is forgotten and requested again after 5 s.
//
// The actor is coop_puppet_actor, by default PouchPlayer, the copy of the player the inventory screen shows.
//
// What it wears: the player's default look first (coop_puppet_hair = 3, below in coop_puppet_present), then the
// peer's own pieces. Each side notes what the game puts on its player (host_piece_note), publishes that table
// (equip_publish), the link sends it (coop.cpp, "TKCE"), and the receiving side puts each differing piece on the puppet
// through the same function. Tried with TOTK_COOP_EQUIP_TEST and coop_peer = loop: armour, a sword on the back, a
// change of both while running, weapons (slot 0 the sword, 7 the bow, 8 the shield, through the game's weapon
// function) and taking them off again show on the puppet. A piece is taken off by hiding its actor, since the game's
// functions only put pieces on; the quiver and the sheath follow the bow and the weapon, as on the player. The saves
// at hand hold only the decayed sword of the game's start, so a real weapon change or break of the player is not
// yet seen.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "cpu.h"
#include "host.h"
#include "layout.h"
#if __has_include("patches.h")
#include "patches.h"
#endif
#include "platform/platform.h"

#define FN_CREATE (MOD_MAIN_BASE + 0x599670)
#define FN_FORCE (MOD_MAIN_BASE + 0x45ce70)
#define FN_DRESS (MOD_MAIN_BASE + 0x1b6cd78)   // the equipment component's "make this slot's piece for a PouchPlayer"
#define FN_WEAPON (MOD_MAIN_BASE + 0xaf100c)   // the player's weapons (main+0xc4ae90)
#define FN_PIECE (MOD_MAIN_BASE + 0xc48ac0)   // the same for the player in the world (the component's start, main+0xc470dc)
#define ACTOR_MGR (MOD_MAIN_BASE + 0x3ad7450)   // ActorMgr::sInstance
#define ACTOR_NAME 0x218
#define SPAWN_RANGE_M 120.0f      // the peer further than this from this side's player: no request, no move
#define RETRY_NS 5000000000ull
#define REMOTE_MAX_AGE_NS 500000000ull
#define HIDE_AFTER_NS 3000000000ull   // no pose from the peer for this long: the puppet is put out of sight
#define HIDE_UP_M 3000.0f             // how far above this side's player, higher than any sky island

typedef struct {
    float pos[3], rot[9], scale[3];
    uint32_t pad3c;
    const char *str40;   // a SafeString the game's callers point at an empty string
    uint64_t z48;
    uint32_t z50, pad54;
    uint64_t blackboard, z60, parent, dependent, creator, creator_rtti, heap;
    uint8_t transform_flags;   // 1 position, 2 rotation, 4 scale
    uint8_t pad91[7];
} CreateArg;
_Static_assert(sizeof(CreateArg) == 0x98, "CreateArg is 0x98 bytes in the game");

// What the request points the game at stays valid for the life of the process: the creation runs later, on another
// thread.
static char name_buf[64];
static const char *name_ref = name_buf;   // the SafeString
static CreateArg arg;
static uint32_t result;
static uint64_t preactor;   // the game writes the request's PreActor here

static uint64_t actor, retry_ns;
static int state;   // 0 nothing asked, 1 requested, 2 the actor is known, -1 off for this run

static uint64_t read_ptr(uint64_t a) {
    uint64_t p = 0;
    return a && plat_mem_read(a, &p, 8) ? p : 0;
}

static int words_are(uint64_t fn, const uint32_t want[4]) {
    uint32_t got[4];
    return plat_mem_read(fn, got, sizeof got) && !memcmp(got, want, sizeof got);
}

static int named(uint64_t a) {
    char text[sizeof name_buf];
    size_t n = strlen(name_buf) + 1;
    uint64_t name = read_ptr(a + ACTOR_NAME);
    return name && plat_mem_read(name, text, n) && !memcmp(text, name_buf, n);
}

static int mode(void) {
    static int m = -2;
    if (m != -2) return m;
    m = 0;
    int want = cfg_int("TOTK_COOP_PUPPET");
    if (want <= 0 && coop_session()) want = 1;   // a session from the start menu shows the peer as an actor
    if (!RT_V143 || want <= 0) return m;
    static const uint32_t create[4] = {0xa9ba7bfd, 0xa9016ffc, 0x910003fd, 0xa90267fa},
                          force[4] = {0xa9bc7bfd, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    if (!words_are(FN_CREATE, create) || !words_are(FN_FORCE, force)) {
        rt_log("coop: puppet: off, this build's actor functions are not the ones of 1.4.3");
        return m;
    }
    const char *name = cfg_value("TOTK_COOP_PUPPET_ACTOR");
    snprintf(name_buf, sizeof name_buf, "%s", name && *name ? name : "PouchPlayer");
    m = want;
    rt_log("coop: puppet: on, the peer is shown as the actor %s (experimental: calls into the game)", name_buf);
    return m;
}

static void matrix_of(const float pos[3], float yaw, float m[12]) {
    float s = sinf(yaw), c = cosf(yaw);   // yaw 0 faces +z (coop_player: atan2(dx, dz))
    float r[12] = {c, 0, s, pos[0], 0, 1, 0, pos[1], -s, 0, c, pos[2]};
    memcpy(m, r, sizeof r);
}

static void request(Cpu *c, const float pos[3], float yaw) {
    uint64_t mgr = read_ptr(ACTOR_MGR);
    if (!mgr) return;   // before the game made its actor manager
    float m[12];
    matrix_of(pos, yaw, m);
    memset(&arg, 0, sizeof arg);
    memcpy(arg.pos, pos, sizeof arg.pos);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) arg.rot[r * 3 + k] = m[r * 4 + k];
    arg.scale[0] = arg.scale[1] = arg.scale[2] = 1;
    arg.str40 = "";
    arg.transform_flags = 1 | 2 | 4;
    result = 0, preactor = 0;
    // the three stack arguments, below the presenting thread's stack pointer for the length of the call
    uint64_t sp = c->sp, frame = (sp - 0x20) & ~15ull;
    uint64_t *st = (uint64_t *)(uintptr_t)frame;
    st[0] = 0, st[1] = (uint64_t)(uintptr_t)&result, st[2] = (uint64_t)(uintptr_t)&preactor, st[3] = 0;
    c->sp = frame;
    uint64_t ok = rt_call(c, FN_CREATE, 8, mgr, (uint64_t)(uintptr_t)&name_ref, (uint64_t)(uintptr_t)&arg, (uint64_t)0,
                          (uint64_t)1, (uint64_t)0, (uint64_t)0, (uint64_t)0);
    c->sp = sp;
    rt_log("coop: puppet: asked the game for %s at %.1f %.1f %.1f: returned %llu, result %u, PreActor %#llx", name_buf,
           pos[0], pos[1], pos[2], (unsigned long long)(ok & 0xff), result, (unsigned long long)preactor);
    if ((ok & 1) && preactor) {
        state = 1;
    } else {
        retry_ns = plat_now_ns() + RETRY_NS;
    }
}

// The created actor, once the game stored it in the request's PreActor.
static uint64_t created(void) {
    uint64_t words[0x200 / 8];
    if (!plat_mem_read(preactor, words, sizeof words)) return 0;
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
        uint64_t p = words[i];
        if (p > 0x10000 && !(p & 7) && p != preactor && named(p)) return p;
    }
    return 0;
}

// ---- parts: further actors that belong to the puppet (the hair, which is an armour actor of its own in the game:
// Armor_Default_Extra_00). Each is requested like the puppet, with the puppet in the CreateArg's parent field, once
// the puppet exists. EXPERIMENT, TOTK_COOP_PUPPET_HAIR: 1 requests it with the parent set and leaves it alone, 2
// requests it without a parent and puts it at the puppet's matrix each present, 3 does both.
typedef struct {
    char name[64];
    const char *ref;
    CreateArg arg;
    uint32_t result;
    uint64_t preactor, actor, retry_ns;
    int state;
} Part;
__attribute__((unused)) static Part hair;   // part_present's first part

static int part_named(const Part *p, uint64_t a) {
    char text[sizeof p->name];
    size_t n = strlen(p->name) + 1;
    uint64_t name = read_ptr(a + ACTOR_NAME);
    return name && plat_mem_read(name, text, n) && !memcmp(text, p->name, n);
}

__attribute__((unused)) static void part_present(Cpu *c, Part *p, const char *name, const float m[12], uint64_t parent, uint64_t slot, int follow) {
    uint64_t now = plat_now_ns(), mgr = read_ptr(ACTOR_MGR);
    if (p->state == 0) {
        if (now < p->retry_ns || !mgr) return;
        snprintf(p->name, sizeof p->name, "%s", name);
        p->ref = p->name;
        memset(&p->arg, 0, sizeof p->arg);
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) p->arg.rot[r * 3 + k] = m[r * 4 + k];
            p->arg.pos[r] = m[r * 4 + 3];
        }
        p->arg.scale[0] = p->arg.scale[1] = p->arg.scale[2] = 1;
        p->arg.str40 = "";
        p->arg.parent = parent, p->arg.creator = parent;
        p->arg.transform_flags = 1 | 2 | 4;
        p->result = 0, p->preactor = 0;
        uint64_t sp = c->sp, frame = (sp - 0x20) & ~15ull;
        uint64_t *st = (uint64_t *)(uintptr_t)frame;
        st[0] = 0, st[1] = (uint64_t)(uintptr_t)&p->result, st[2] = (uint64_t)(uintptr_t)&p->preactor, st[3] = 0;
        c->sp = frame;
        // The callback the game's equipment code passes for the player's own pieces (main+0xc49cb0): an object of the
        // class main+0x36727e0 whose other words that code leaves unset, so the class is all of it.
        static uint64_t equip_callback[8];
        equip_callback[0] = MOD_MAIN_BASE + 0x36727e0;
        uint64_t ok = rt_call(c, FN_CREATE, 8, mgr, (uint64_t)(uintptr_t)&p->ref, (uint64_t)(uintptr_t)&p->arg, slot,
                              (uint64_t)0, (uint64_t)0, (uint64_t)0, slot ? (uint64_t)(uintptr_t)equip_callback : (uint64_t)0);
        c->sp = sp;
        rt_log("coop: puppet: asked the game for the part %s (parent %#llx): returned %llu, result %u, PreActor %#llx",
               p->name, (unsigned long long)parent, (unsigned long long)(ok & 0xff), p->result,
               (unsigned long long)p->preactor);
        if ((ok & 1) && p->preactor) p->state = 1;
        p->retry_ns = now + 2 * RETRY_NS;
        return;
    }
    if (p->state == 1) {
        uint64_t words[0x200 / 8];
        if (plat_mem_read(p->preactor, words, sizeof words))
            for (unsigned i = 0; i < sizeof words / sizeof *words && !p->actor; i++)
                if (words[i] > 0x10000 && !(words[i] & 7) && words[i] != p->preactor && part_named(p, words[i]))
                    p->actor = words[i];
        if (p->actor) {
            p->state = 2;
            rt_log("coop: puppet: the game created the part %s, actor %#llx", p->name, (unsigned long long)p->actor);
        } else if (now >= p->retry_ns) {
            rt_log("coop: puppet: no actor for the part %s after 10 s", p->name);
            p->state = 0;
        }
        return;
    }
    if (!part_named(p, p->actor)) {
        p->state = 0, p->actor = 0, p->retry_ns = now + RETRY_NS;
        return;
    }
    if (follow) rt_call(c, FN_FORCE, 3, p->actor, (uint64_t)(uintptr_t)m, (uint64_t)0);
}

// ---- the pose. Both the player's actor and the puppet hold their skeletons in a block of objects of the class
// main+0x3909cd0, 0xb0 bytes apart from the block's +0x18: one of 63 bones and one of 35 (the body and the face; the
// two actors hold them in opposite order), the rest empty. The block is [[component 1 + 0x310] + 0x130] in the
// player and [[component 1 + 0x2f8] + 0x58] in PouchPlayer (found by the diag's search below, the same in every run).
// In such an object +0x48 points at the bones' local transforms, 0x30 bytes a bone (a flag word, scale, translation,
// rotation), and the u16 at +0x64 is the bone count. The puppet has no animation component (the player's component
// 43), so nothing else writes its local transforms: copying the player's over them gives the puppet the player's
// pose. Each skeleton is matched by its bone count, and an actor whose block does not read back as described keeps
// its pose.
#define SKEL_CLASS (MOD_MAIN_BASE + 0x3909cd0)
#define SKEL_STRIDE 0xb0
#define VT_WORDS 26   // two words before the class's function table (offset, type info), then its slots
static uint64_t vt_copy[VT_WORDS];   // the table with slot 3 replaced, for the puppet's two objects (pose_hook)
#define SKEL_COPY ((uint64_t)(uintptr_t)&vt_copy[2])
static int is_skel(uint64_t cls) { return cls == SKEL_CLASS || cls == SKEL_COPY; }
#define LOCAL_BYTES 0x30
#define MAX_BONES 128

static uint64_t skel_block(uint64_t a, int is_player) {
    uint64_t comp = read_ptr(read_ptr(a + 0x228) + 8);
    return comp ? read_ptr(read_ptr(comp + (is_player ? 0x310 : 0x2f8)) + (is_player ? 0x130 : 0x58)) : 0;
}

// The local transforms of the block's skeleton with `bones` bones, 0 when it has none.
static uint64_t skel_locals(uint64_t block, unsigned bones) {
    for (int i = 0; block && i < 5; i++) {
        uint64_t s = block + 0x18 + (uint64_t)i * SKEL_STRIDE;
        uint16_t count = 0;
        if (!is_skel(read_ptr(s)) || !plat_mem_read(s + 0x64, &count, 2) || count != bones) continue;
        return read_ptr(s + 0x48);
    }
    return 0;
}

// A bone's local transform: a flag word, scale (3 floats), translation (3), a pad word, rotation (4). The link carries
// the translation and the rotation (coop.h COOP_BONE_VALUES): each within +-8, which a bone's offset in metres and its
// rotation's numbers are, in steps of 1/4096.
static const unsigned skel_bones[2] = {63, 35};

static int16_t quant(float f) {
    if (!(f == f)) return 0;
    float s = f * 4096.0f;
    return s > 32767.0f ? 32767 : s < -32767.0f ? -32767 : (int16_t)lrintf(s);
}

// This side's pose to the link (coop_publish_bones), 30 times a second. 1 when both skeletons were read.
static int pose_publish(uint64_t player) {
    static int16_t v[COOP_BONE_VALUES];
    uint64_t block = skel_block(player, 1);
    int16_t *out = v;
    for (int i = 0; i < 2; i++) {
        uint8_t buf[MAX_BONES * LOCAL_BYTES];
        uint64_t src = skel_locals(block, skel_bones[i]);
        if (!src || !plat_mem_read(src, buf, skel_bones[i] * LOCAL_BYTES)) return 0;
        for (unsigned b = 0; b < skel_bones[i]; b++) {
            float f[12];
            memcpy(f, buf + b * LOCAL_BYTES, sizeof f);
            uint32_t flag;
            memcpy(&flag, f, 4);
            *out++ = (int16_t)(uint16_t)flag, *out++ = (int16_t)(uint16_t)(flag >> 16);
            for (int k = 0; k < 3; k++) *out++ = quant(f[4 + k]);
            for (int k = 0; k < 4; k++) *out++ = quant(f[8 + k]);
        }
    }
    // The bones hang on the model's matrix, which is not always the actor's: while climbing the game turns the model
    // against the wall and leaves the actor upright. The model is [component 1 + 0x28] and its matrix (3x4 in rows)
    // is at +0x1f8 (lotuskit engineComponent.hpp: ModelComponent::model, Model::modelMtx). A matrix that does not
    // read as one near the actor gives way to the actor's rotation.
    float rot[9], mm[12], pos[3];
    if (!plat_mem_read(player + 0x2c0, rot, sizeof rot) || !plat_mem_read(player + 0x2b4, pos, sizeof pos)) return 0;
    uint64_t model = read_ptr(read_ptr(read_ptr(player + 0x228) + 8) + 0x28);
    float off[3] = {0, 0, 0};
    if (model && plat_mem_read(model + 0x1f8, mm, sizeof mm)) {
        int ok = 1;
        for (int row = 0; ok && row < 3; row++) {
            float len = mm[row * 4] * mm[row * 4] + mm[row * 4 + 1] * mm[row * 4 + 1] + mm[row * 4 + 2] * mm[row * 4 + 2];
            float d = mm[row * 4 + 3] - pos[row];
            ok = len > 0.9f && len < 1.1f && d > -5.0f && d < 5.0f;
        }
        for (int row = 0; ok && row < 3; row++) {
            for (int k = 0; k < 3; k++) rot[row * 3 + k] = mm[row * 4 + k];
            off[row] = mm[row * 4 + 3] - pos[row];
        }
    }
    for (int k = 0; k < 9; k++) *out++ = quant(rot[k]);
    for (int k = 0; k < 3; k++) *out++ = quant(off[k]);
    coop_publish_bones(v);
    return 1;
}

// The actor matrix for the peer's pose: its rotation as sent when that reads as a rotation (rows of length 1),
// else the pose's yaw.
static void matrix_of_rot(const float pos[3], float yaw, const int16_t *q, float m[12]) {
    float r[9];
    int ok = q != NULL;
    for (int row = 0; ok && row < 3; row++) {
        float len = 0;
        for (int k = 0; k < 3; k++) r[row * 3 + k] = (float)q[row * 3 + k] / 4096.0f, len += r[row * 3 + k] * r[row * 3 + k];
        ok = len > 0.9f && len < 1.1f;
    }
    if (!ok) {
        matrix_of(pos, yaw, m);
        return;
    }
    for (int row = 0; row < 3; row++) {
        for (int k = 0; k < 3; k++) m[row * 4 + k] = r[row * 3 + k];
        m[row * 4 + 3] = pos[row] + (float)q[9 + row] / 4096.0f;   // the model's offset from the actor
    }
}

// ---- the peer's pose into the puppet. The puppet plays an animation of its own (the pose of the inventory screen),
// which the game's worker threads write into its local transforms every frame, at any time relative to the present:
// what the presenting thread writes there is overwritten before the skeleton is worked out. So the pose goes in
// behind the game's animation step. The objects of the class main+0x3909cd0 (above) are the animation's result
// buffers: slot 3 of the class's function table, main+0x283a630, works the animation out into the locals (+0x48)
// when the frame changed, and the game then copies them into the model's skeleton. The puppet's two objects, and only
// those, get a copy of the table whose slot 3 is host_anim_calc: it runs the game's function and then writes the
// peer's pose over the result. The copy is made once, after checking that the class's slot 3 is that function.
//
// How the place was found, and what does not work: writing the locals at the present is overwritten within half a
// millisecond (the puppet then shows a mix of both poses, upside down while the player climbs); the actor's pause
// bit (+0x5ac bit 0x40) does not stop the puppet's animation; slot 6 (main+0x283a440) and main+0x283ea40 of the
// neighbouring table are never called for these objects. TOTK_COOP_PUPPET_DIAG=w (below) made the locals' pages
// read-only and named the one writer: the game's memcpy, called from slot 3's animation step.//
// The presenting thread hands the pose over in two buffers it fills in turn; pose_at says which is complete. A
// reader on a worker thread that is overtaken twice during its copy mixes two poses for one frame.
#define FN_ANIM_CALC (MOD_MAIN_BASE + 0x283a630)
#define ANIM_CALC_SLOT 3

static _Atomic uint64_t puppet_skel[2];   // the puppet's skeletons of 63 and of 35 bones, 0 while not known
static int16_t pose_buf[2][COOP_BONE_VALUES];
static _Atomic int pose_at = -1;
static _Atomic uint64_t pose_ns;
static _Atomic uint32_t calc_calls;

// The flag word goes with a bone's numbers: it tells the game which parts of the transform are zero or one and may
// be skipped. Scale stays the puppet's own.
static void pose_write(uint64_t locals, unsigned bones, const int16_t *in) {
    for (unsigned b = 0; b < bones; b++) {
        float *f = (float *)(uintptr_t)(locals + (uint64_t)b * LOCAL_BYTES);
        uint32_t flag = (uint16_t)in[0] | (uint32_t)(uint16_t)in[1] << 16;
        in += 2;
        memcpy(f, &flag, 4);
        for (int k = 0; k < 3; k++) f[4 + k] = (float)*in++ / 4096.0f;
        for (int k = 0; k < 4; k++) f[8 + k] = (float)*in++ / 4096.0f;
    }
}

#ifdef RT_PATCH_PAGE
#define HOST_ANIM_CALC (RT_PATCH_PAGE + 0xf60)   // free on the patch page: mouse_direct.c has 0xf70 and up

// In place of the class's slot 3 for the puppet's two objects, on the game thread that works the animation out: the
// game's function first, then the peer's pose over its result. x0 is the object; the other registers pass through.
static void host_anim_calc(Cpu *c) {
    uint64_t s = c->x[0];
    rt_call(c, FN_ANIM_CALC, 0);
    int which = s == atomic_load_explicit(&puppet_skel[0], memory_order_relaxed) ? 0
              : s == atomic_load_explicit(&puppet_skel[1], memory_order_relaxed) ? 1 : -1;
    if (which < 0 || !s) return;
    int at = atomic_load_explicit(&pose_at, memory_order_acquire);
    uint16_t count = *(const uint16_t *)(uintptr_t)(s + 0x64);
    uint64_t locals = *(const uint64_t *)(uintptr_t)(s + 0x48);
    if (at >= 0 && locals && count == skel_bones[which] &&
        plat_now_ns() - atomic_load_explicit(&pose_ns, memory_order_relaxed) < REMOTE_MAX_AGE_NS)
        pose_write(locals, count, pose_buf[at] + (which ? skel_bones[0] * COOP_BONE_FIELDS : 0));
    atomic_fetch_add_explicit(&calc_calls, 1, memory_order_relaxed);
}

// Bit 0x8000 of actor +0x5ac. The game's actor update clears it (with 0x4000) at main+0x426cb0 and sets it again on
// some paths (main+0x426d74); PouchPlayer's body is drawn only while it is set (without it the puppet vanishes). The
// patch set puppet_flags (recomp/patches-v143.txt) turns the store of the cleared word into a bl here: x9 is the actor,
// w8 the word. EXPERIMENT, only with TOTK_COOP_PUPPET_DIAG containing 'a' (the set stays off otherwise): each armour
// piece's words are logged once, and a piece whose model lacks the player's bits gets the player's hair values, here
// inside the update rather than at the present. Results (2026-11-12, coop_puppet_hair = 1): the puppet's two hair pieces
// do pass this update (so they are updated), with +0x5ac 0x1 (the player's: 0x120), +0x440 0xc34301 (0x530300) and the
// model words +0x240 0x10011 (0x16810), +0x274 0x320000 (0x720000), +0x2ac and +0x324 0 (1). Bit 0x8000 alone, and all
// of these values together, written here each frame: the hair is still not drawn. Letting the puppet's body go
// without 0x8000 hides the body. Next suspect: the pieces' binding to the body's bones (lotuskit exkingEnums.hpp
// names SameBoneModelBindRef), which the inventory screen may make and the world does not.
#define HOST_PUPPET_FLAG (RT_PATCH_PAGE + 0xf50)
static _Atomic uint64_t puppet_actor;   // the puppet while it is known
static _Atomic uint32_t flag_forced;
static int flag_mode;   // 'a': force Armor_*

static void host_puppet_flag(Cpu *c) {
    uint64_t a = c->x[9];
    uint32_t w = (uint32_t)c->x[8];
    if (flag_mode == 'a') {
        uint64_t np = *(const uint64_t *)(uintptr_t)(a + ACTOR_NAME);
        const char *n = (const char *)(uintptr_t)np;
        if (n && !strncmp(n, "Armor_", 6)) {
            // the player's hair values (the 'h' diag's comparison) on every armour piece; on the player's own pieces
            // they are the values already there
            uint64_t model = 0, comp = *(const uint64_t *)(uintptr_t)(a + 0x228);
            if (comp) comp = *(const uint64_t *)(uintptr_t)(comp + 8);
            if (comp) model = *(const uint64_t *)(uintptr_t)(comp + 0x28);
            static uint64_t seen[16];
            static _Atomic int nseen;
            int k, ns = atomic_load(&nseen);
            for (k = 0; k < ns && seen[k] != a; k++) {}
            if (k == ns && ns < 16) {
                seen[ns] = a, atomic_store(&nseen, ns + 1);
                rt_log("coop: puppet diag: armour %#llx %.40s: +0x5ac %08x +0x440 %08x, model %#llx +0x240 %08x +0x274 %08x "
                       "+0x2ac %x +0x324 %x", (unsigned long long)a, n, w, *(const uint32_t *)(uintptr_t)(a + 0x440),
                       (unsigned long long)model, model ? *(const uint32_t *)(uintptr_t)(model + 0x240) : 0,
                       model ? *(const uint32_t *)(uintptr_t)(model + 0x274) : 0,
                       model ? *(const uint32_t *)(uintptr_t)(model + 0x2ac) : 0,
                       model ? *(const uint32_t *)(uintptr_t)(model + 0x324) : 0);
            }
            if (model && (*(const uint32_t *)(uintptr_t)(model + 0x240) & 0x6800) != 0x6800) {
                w = (w & ~1u) | 0x120;
                *(uint32_t *)(uintptr_t)(a + 0x440) = 0x530300;
                *(uint32_t *)(uintptr_t)(a + 0x1d4) |= 3, *(uint32_t *)(uintptr_t)(a + 0x530) = 1;
                *(uint32_t *)(uintptr_t)(model + 0x2ac) = 1, *(uint32_t *)(uintptr_t)(model + 0x324) = 1;
                *(uint32_t *)(uintptr_t)(model + 0x274) |= 0x400000, *(uint32_t *)(uintptr_t)(model + 0x240) = (*(const uint32_t *)(uintptr_t)(model + 0x240) | 0x6800) & ~1u;
                atomic_fetch_add_explicit(&flag_forced, 1, memory_order_relaxed);
            }
        }
        static _Atomic int logged;
        if (n && atomic_fetch_add(&logged, 1) < 0) rt_log("coop: puppet diag: flag store for %#llx %.40s (%08x)",
                                                               (unsigned long long)a, n, w);
    }
    *(uint32_t *)(uintptr_t)(a + 0x5ac) = w;
}

// What the player wears, for the peer's puppet. The game dresses the player in the world through its piece function
// main+0xc48ac0 (equipment component x0, slot w1, x2 the address of a pointer to the piece's name, w3, w4, w5): at a
// load (main+0xc470dc and on: hair, Zonai devices, arm, quiver; main+0xc4aba0: head, upper and lower armour, w4 1;
// main+0xc4ae38: the sheath) and when the player changes a piece. The patch set piece_note (recomp/patches-v143.txt)
// turns its instruction at main+0xc48ae0, and x21, x3 & 0xffffffff (done here first), into a bl here, after the prologue
// saved x30. Each call is noted per component in one of PIECE_TABLES tables (the player's component is only known at
// the next present, and a load makes a new one); coop_puppet_present publishes the player's table, the link sends it.
#define HOST_PIECE_NOTE (RT_PATCH_PAGE + 0xf40)
#define PIECE_TABLES 4
static struct {
    uint64_t comp;
    uint32_t gen;   // counts the changes
    uint32_t calls[COOP_EQUIP_SLOTS];   // counts the calls per slot: a weapon put on again under the same name is new
    CoopEquip e;
} piece_tab[PIECE_TABLES];
static unsigned piece_next;
static atomic_flag piece_lock = ATOMIC_FLAG_INIT;
static _Atomic uint64_t puppet_equip;   // the puppet's component: the calls made here for it are not noted
static char puppet_asked[COOP_EQUIP_SLOTS][COOP_EQUIP_NAME];   // the presenting thread: pieces asked for the puppet
static uint32_t puppet_asked_mask;

static int piece_named(uint64_t a, const char *want) {
    char text[COOP_EQUIP_NAME];
    size_t n = strlen(want) + 1;
    uint64_t name = read_ptr(a + ACTOR_NAME);
    return n > 1 && name && plat_mem_read(name, text, n) && !memcmp(text, want, n);
}

static uint64_t piece_model(uint64_t a) { return read_ptr(read_ptr(read_ptr(a + 0x228) + 8) + 0x28); }

// The actor named want among the words of a request (a PreActor) or one pointer further: 0 while it is not made.
static uint64_t find_named(const uint64_t *words, unsigned n, uint64_t self, const char *want) {
    for (unsigned i = 0; i < n; i++) {
        uint64_t w = words[i], inner[0x80 / 8];
        if (w < 0x10000 || (w & 7) || w == self) continue;
        if (piece_named(w, want)) return w;
        if (plat_mem_read(w, inner, sizeof inner))   // some requests hold a handle
            for (unsigned k = 0; k < sizeof inner / 8; k++)
                if (inner[k] > 0x10000 && !(inner[k] & 7) && inner[k] != w && piece_named(inner[k], want)) return inner[k];
    }
    return 0;
}

static void piece_note(Cpu *c, int weapon_call);
static void host_piece_note(Cpu *c) {
    c->x[21] = c->x[3] & 0xffffffffull;
    piece_note(c, 0);
}
// The weapons take another function, main+0xaf100c (component x0, slot w1, x2 the address of a pointer to the name,
// and more): its instruction at main+0xaf1030, mov w8, #0x43a4 (done here first), becomes a bl to page +0xf30.
#define HOST_WEAPON_NOTE (RT_PATCH_PAGE + 0xf30)
static void host_weapon_note(Cpu *c) {
    c->x[8] = 0x43a4;
    static _Atomic int wl;
    if (getenv("TOTK_COOP_WEAPON_LOG") && atomic_fetch_add(&wl, 1) < 12) {
        uint64_t st[6] = {0}, s6[4] = {0}, s0w[4] = {0};
        char n2[48] = {0}, n6[48] = {0}, n0[48] = {0};
        plat_mem_read(c->x[29] + 0x60, st, sizeof st), plat_mem_read(c->x[6], s6, sizeof s6), plat_mem_read(st[0], s0w, sizeof s0w);
        plat_mem_read(read_ptr(c->x[2]), n2, 47), plat_mem_read(read_ptr(c->x[6]), n6, 47), plat_mem_read(read_ptr(st[0]), n0, 47);
        for (int i = 0; i < 47; i++) {
            if (n2[i] && (n2[i] < 32 || n2[i] > 126)) n2[i] = '.';
            if (n6[i] && (n6[i] < 32 || n6[i] > 126)) n6[i] = '.';
            if (n0[i] && (n0[i] < 32 || n0[i] > 126)) n0[i] = '.';
        }
        rt_log("coop: weapon: comp %llx slot %llu x2->[%s] x3 %llx x4 %llx x5 %llx x6 %llx {%llx %llx %llx %llx}->[%s] x7 %llx "
               "stack %llx %llx %llx %llx %llx %llx [st0]{%llx %llx %llx %llx}->[%s] lr main+%llx",
               (unsigned long long)c->x[0], (unsigned long long)(c->x[1] & 0xffffffff), n2, (unsigned long long)c->x[3],
               (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)s6[0],
               (unsigned long long)s6[1], (unsigned long long)s6[2], (unsigned long long)s6[3], n6, (unsigned long long)c->x[7],
               (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3],
               (unsigned long long)st[4], (unsigned long long)st[5], (unsigned long long)s0w[0], (unsigned long long)s0w[1],
               (unsigned long long)s0w[2], (unsigned long long)s0w[3], n0, (unsigned long long)(read_ptr(c->x[29] + 8) - MOD_MAIN_BASE));
    }
    piece_note(c, 1);
}

static void piece_note(Cpu *c, int weapon_call) {
    uint64_t comp = c->x[0];
    unsigned slot = (unsigned)(c->x[1] & 0xffffffff);
    if (!comp || slot >= COOP_EQUIP_SLOTS) return;
    char name[COOP_EQUIP_NAME] = {0};
    uint64_t text = read_ptr(c->x[2]);
    if (text && !plat_mem_read(text, name, sizeof name - 1)) {
        // a name that ends short of a page boundary: read it byte by byte
        for (unsigned i = 0; i < sizeof name - 1 && plat_mem_read(text + i, name + i, 1) && name[i]; i++) {}
    }
    name[sizeof name - 1] = 0;
    if (comp == atomic_load_explicit(&puppet_equip, memory_order_relaxed)) {
        // a piece for the puppet: the calls made in coop_puppet_present and those the game's functions make inside
        // them (the weapon function puts the weapon's own sheath in slot 3); noted so that it can be found and hidden
        memcpy(puppet_asked[slot], name, sizeof name);
        puppet_asked_mask |= 1u << slot;
        return;
    }
    uint8_t flags = (uint8_t)(COOP_EQUIP_NOTED | (c->x[4] & 1 ? COOP_EQUIP_W4 : 0) | (c->x[5] & 1 ? COOP_EQUIP_W5 : 0));
    uint32_t w3 = (uint32_t)c->x[3];
    if (weapon_call) {   // the weapon function's x3, x4 and x5 in one word; COOP_EQUIP_WEAPON marks it
        w3 = (uint32_t)(c->x[3] & 0xff) | (uint32_t)(c->x[4] & 0xffff) << 8 | (uint32_t)(c->x[5] & 0xff) << 24;
        flags |= COOP_EQUIP_WEAPON;
    }
    while (atomic_flag_test_and_set_explicit(&piece_lock, memory_order_acquire)) plat_pause();
    int t = -1;
    for (int i = 0; i < PIECE_TABLES; i++)
        if (piece_tab[i].comp == comp) t = i;
    if (t < 0) {   // a component not seen before: it takes the oldest table
        t = (int)(piece_next++ % PIECE_TABLES);
        memset(&piece_tab[t], 0, sizeof piece_tab[t]);
        piece_tab[t].comp = comp;
    }
    CoopEquip *e = &piece_tab[t].e;
    piece_tab[t].calls[slot]++;
    if (weapon_call || strcmp(e->name[slot], name) || e->flags[slot] != flags || e->w3[slot] != w3) {
        memcpy(e->name[slot], name, sizeof name);
        e->flags[slot] = flags, e->w3[slot] = w3;
        piece_tab[t].gen++;
    }
    atomic_flag_clear_explicit(&piece_lock, memory_order_release);
}

// TOTK_COOP_EQUIP_TEST (development): "slot=name,slot=name" entries put over the player's table before it is
// published, to try pieces the save does not have (with coop_peer = loop they come back as the peer's). Entries after
// a '|' are added 20 s after the first table, to try a change.
static void equip_test_part(CoopEquip *e, const char *t, size_t len);
static void equip_test(CoopEquip *e, int phase) {
    const char *t = getenv("TOTK_COOP_EQUIP_TEST");
    for (int p = 0; t && *t; p++) {
        size_t part = strcspn(t, "|");
        if (p <= phase) equip_test_part(e, t, part);
        t += part;
        if (*t == '|') t++;
    }
}
static void equip_test_part(CoopEquip *e, const char *t, size_t len) {
    const char *stop = t + len;
    while (t < stop) {
        char *end;
        long slot = strtol(t, &end, 10);
        if (end == t || *end != '=' || slot < 0 || slot >= COOP_EQUIP_SLOTS) return;
        t = end + 1;
        size_t n = strcspn(t, ",|");
        if (n >= COOP_EQUIP_NAME) return;
        memcpy(e->name[slot], t, n);
        e->name[slot][n] = 0;
        e->flags[slot] = COOP_EQUIP_NOTED | COOP_EQUIP_W5, e->w3[slot] = 0xffffffffu;
        if (!strncmp(e->name[slot], "Weapon_", 7) && strncmp(e->name[slot], "Weapon_Sheath_", 14) && strncmp(e->name[slot], "Weapon_Quiver_", 14))
            e->flags[slot] = COOP_EQUIP_NOTED | COOP_EQUIP_WEAPON, e->w3[slot] = 2 | 6 << 8;   // as the player's sword
        t += n;
        if (*t == ',') t++;
    }
}

// At the present: the player's table to the link when it changed (a load gives the player a new component).
static uint64_t diag_found, diag_hits[4];
static int diag_nhits;
static void diag_bones(const char *who, uint64_t a, const Part *find);
static struct {
    _Atomic int state;   // 0 idle, 1 running, 2 done
    uint64_t player, result;
    int slot;
    Part want;
} wsearch;

static void weapon_search(void *arg) {
    (void)arg;
    float pp[3] = {0};
    plat_mem_read(wsearch.player + 0x2b4, pp, sizeof pp);
    uint64_t t0 = plat_now_ns(), best = 0;
    float bestd = 4.0f;   // of the hits the one within 2 m of the player (the puppet's pieces are elsewhere)
    diag_nhits = 0, diag_found = 0;
    diag_bones("player (weapon)", wsearch.player, &wsearch.want);
    for (int h = 0; h < diag_nhits; h++) {
        float hp[3];
        if (!piece_named(diag_hits[h], wsearch.want.name) || !plat_mem_read(diag_hits[h] + 0x2b4, hp, sizeof hp)) continue;
        float d = (hp[0] - pp[0]) * (hp[0] - pp[0]) + (hp[1] - pp[1]) * (hp[1] - pp[1]) + (hp[2] - pp[2]) * (hp[2] - pp[2]);
        if (d < bestd) bestd = d, best = diag_hits[h];
    }
    diag_nhits = 0;
    wsearch.result = best;
    rt_log("coop: equip: searched the player's %s: %s %#llx in %.0f ms", wsearch.want.name, best ? "found" : "not found",
           (unsigned long long)best, (double)(plat_now_ns() - t0) / 1e6);
    atomic_store_explicit(&wsearch.state, 2, memory_order_release);
}

static void equip_publish(uint64_t player) {
    static uint64_t pub_comp;
    static uint32_t pub_gen, count;
    static CoopEquip last;
    uint64_t comp = player ? read_ptr(read_ptr(player + 0x228) + 70 * 8) : 0;
    if (!comp || read_ptr(comp) != MOD_MAIN_BASE + 0x3672c18) return;
    static CoopEquip e;
    static uint32_t calls[COOP_EQUIP_SLOTS];
    int found = 0;
    uint32_t gen = 0;
    while (atomic_flag_test_and_set_explicit(&piece_lock, memory_order_acquire)) plat_pause();
    for (int i = 0; i < PIECE_TABLES; i++)
        if (piece_tab[i].comp == comp)
            e = piece_tab[i].e, gen = piece_tab[i].gen, memcpy(calls, piece_tab[i].calls, sizeof calls), found = 1;
    atomic_flag_clear_explicit(&piece_lock, memory_order_release);
    if (!found) return;
    // A weapon that breaks, is thrown or is put away leaves without a call the hooks see: each weapon's actor is
    // followed (found through its slot's request, as for the puppet's pieces), and once it is gone the slot goes out
    // empty until the player puts a weapon there again.
    static struct { uint64_t pre, actor, since; uint32_t calls; int searched; char name[COOP_EQUIP_NAME]; } trk[COOP_EQUIP_SLOTS];
    static uint64_t trk_comp;
    uint64_t now = plat_now_ns();
    uint32_t gone = 0;
    if (trk_comp != comp) trk_comp = comp, memset(trk, 0, sizeof trk);
    for (int s = 0; s < COOP_EQUIP_SLOTS; s++) {
        if (!(e.flags[s] & COOP_EQUIP_WEAPON) || !e.name[s][0]) continue;
        if (trk[s].calls != calls[s] || strcmp(trk[s].name, e.name[s])) {
            memset(&trk[s], 0, sizeof trk[s]);
            trk[s].calls = calls[s], trk[s].since = now;
            memcpy(trk[s].name, e.name[s], COOP_EQUIP_NAME);
        }
        if (!trk[s].actor && trk[s].since) {
            uint64_t cur = read_ptr(comp + 0x1430 + (uint64_t)s * 8), words[0x200 / 8];
            if (cur) trk[s].pre = cur;
            if (now - trk[s].since > 10000000000ull && trk[s].searched) trk[s].since = 0;
            else if (trk[s].pre && plat_mem_read(trk[s].pre, words, sizeof words)) trk[s].actor = find_named(words, sizeof words / 8, trk[s].pre, trk[s].name);
            if (!trk[s].actor && trk[s].searched < 2 && now - trk[s].since > 2000000000ull) {
                // put on before this followed it (at a load): searched once from the player's components, as the
                // diagnostics find a part by name, on a thread of its own (it takes some tenths of a second)
                int js = atomic_load_explicit(&wsearch.state, memory_order_acquire);
                if (!trk[s].searched && js != 1) {
                    trk[s].searched = 1;
                    wsearch.player = player, wsearch.slot = s, wsearch.result = 0;
                    snprintf(wsearch.want.name, sizeof wsearch.want.name, "%s", trk[s].name);
                    atomic_store_explicit(&wsearch.state, 1, memory_order_release);
                    if (!plat_thread_spawn(weapon_search, NULL, "coop weapon search", 0)) atomic_store(&wsearch.state, 2);
                } else if (trk[s].searched == 1 && js == 2 && wsearch.slot == s) {
                    trk[s].searched = 2, trk[s].actor = wsearch.result;
                    atomic_store(&wsearch.state, 0);
                }
            }
        }
        int32_t st = 0;
        if (trk[s].actor && (!piece_named(trk[s].actor, trk[s].name) ||
                             (plat_mem_read(trk[s].actor + 0x24, &st, 4) && st >= 7 && st <= 11)))
            gone |= 1u << s;
    }
    static uint32_t pub_gone;
    static uint64_t first_ns;
    static int pub_phase;
    int phase = first_ns && now - first_ns > 20000000000ull;
    if (comp == pub_comp && gen == pub_gen && phase == pub_phase && gone == pub_gone) return;
    if (!first_ns) first_ns = now;
    if (gone != pub_gone) rt_log("coop: equip: the player's weapons gone from the slots %#x", gone);
    pub_comp = comp, pub_gen = gen, pub_phase = phase, pub_gone = gone;
    for (int s = 0; s < COOP_EQUIP_SLOTS; s++)
        if (gone & 1u << s) e.name[s][0] = 0;
    equip_test(&e, phase);
    if (!count) count = (uint32_t)(plat_now_ns() >> 20) | 1;   // a restarted game must not repeat its last gen
    e.gen = ++count;
    static int lines;
    for (int s = 0; s < COOP_EQUIP_SLOTS && lines < 60; s++)
        if ((e.flags[s] & COOP_EQUIP_NOTED) && (strcmp(e.name[s], last.name[s]) || !(last.flags[s] & COOP_EQUIP_NOTED)))
            lines++, rt_log("coop: equip: the player wears %s in slot %d", e.name[s][0] ? e.name[s] : "(nothing)", s);
    last = e;
    coop_publish_equip(&e);
}

__attribute__((constructor)) static void puppet_register(void) {
    rt_register_host(HOST_ANIM_CALC, host_anim_calc);
    rt_register_host(HOST_PUPPET_FLAG, host_puppet_flag);
    rt_register_host(HOST_PIECE_NOTE, host_piece_note);
    rt_register_host(HOST_WEAPON_NOTE, host_weapon_note);
}

// The puppet for host_puppet_flag (0: none). The first call with a puppet switches the patch set on when an
// experiment wants it.
static void unpause_note(uint64_t a) {
    static int switched;
    atomic_store_explicit(&puppet_actor, a, memory_order_relaxed);
    if (a && !switched) {
        switched = 1;
        const char *d = cfg_get("TOTK_COOP_PUPPET_DIAG");
        flag_mode = d && strchr(d, 'a') ? 'a' : 0;
        int set = rt_patch_set("puppet_flags");
        if (set >= 0 && flag_mode) rt_patch_switch(set, 1);
        rt_log("coop: puppet: flag hook %s", rt_patch_enabled(set) ? "on" : "off");
    }
}

// Points the puppet's two objects at the table copy and notes them for host_anim_calc. Returns how many of the two
// have it; puppet 0 forgets them (their memory is the game's to free).
static int pose_hook(uint64_t puppet) {
    uint64_t block = puppet ? skel_block(puppet, 0) : 0, found[2] = {0, 0};
    if (block && !vt_copy[2]) {
        uint64_t t[VT_WORDS];
        if (!plat_mem_read(SKEL_CLASS - 16, t, sizeof t) || t[2 + ANIM_CALC_SLOT] != FN_ANIM_CALC) return 0;
        t[2 + ANIM_CALC_SLOT] = HOST_ANIM_CALC;
        memcpy(vt_copy, t, sizeof t);
    }
    for (int i = 0; block && i < 5; i++) {
        uint64_t s = block + 0x18 + (uint64_t)i * SKEL_STRIDE, cls = read_ptr(s);
        uint16_t count = 0;
        if (!is_skel(cls) || !plat_mem_read(s + 0x64, &count, 2)) continue;
        int which = count == skel_bones[0] ? 0 : count == skel_bones[1] ? 1 : -1;
        if (which < 0) continue;
        found[which] = s;
    }
    atomic_store_explicit(&puppet_skel[0], found[0], memory_order_relaxed);
    atomic_store_explicit(&puppet_skel[1], found[1], memory_order_relaxed);
    for (int i = 0; i < 2; i++)
        if (found[i] && read_ptr(found[i]) == SKEL_CLASS) *(uint64_t *)(uintptr_t)found[i] = SKEL_COPY;
    return (found[0] != 0) + (found[1] != 0);
}
#else
static int pose_hook(uint64_t puppet) { (void)puppet; return 0; }
static void unpause_note(uint64_t a) { (void)a; }
#endif
// The presenting thread: the peer's newest pose for host_skel_calc.
static void pose_store(const int16_t *v) {
    int next = atomic_load_explicit(&pose_at, memory_order_relaxed) == 0 ? 1 : 0;
    memcpy(pose_buf[next], v, sizeof pose_buf[next]);
    atomic_store_explicit(&pose_ns, plat_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&pose_at, next, memory_order_release);
}

// TOTK_COOP_PUPPET_DIAG=w (Windows): who writes the puppet's local transforms. The pages holding the body
// skeleton's locals are made read-only at each present; the first write to them faults, the handler notes the
// faulting instruction (as an offset in totk.exe, for totk.map) when the write lies inside the locals, with the
// return addresses found on that thread's stack, and gives the pages back. After 8 s the log lists the writers.
#ifdef _WIN32
#include <windows.h>
#define WATCH_MAX 48
typedef struct { uint64_t rip, hits, first_off, sets, stack[6], guest[6]; } WatchHit;
static _Atomic uint32_t watch_bit;   // 0, or the bit of the watched u32 whose setting is counted (WatchHit.sets)
static _Atomic int watch_keep;       // 1: the pages are protected again after each write (one single step)
static __thread int step_hit = -2;   // this thread is stepping over a write: -1 one outside the range, else its hit
static __thread uint32_t step_old;
static WatchHit watch_hits[WATCH_MAX];
static _Atomic int watch_n;
static _Atomic uint64_t watch_lo, watch_hi, watch_page, watch_len, watch_other;
static uint64_t image_lo, image_hi;

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && step_hit != -2) {   // the write is done: count it, protect again
        uint64_t page = atomic_load(&watch_page), len = atomic_load(&watch_len);
        uint32_t bit = atomic_load(&watch_bit);
        if (step_hit >= 0 && bit && len && !(step_old & bit) && (*(const uint32_t *)(uintptr_t)atomic_load(&watch_lo) & bit))
            watch_hits[step_hit].sets++;
        step_hit = -2;
        DWORD old2;
        if (len) VirtualProtect((void *)(uintptr_t)page, (SIZE_T)len, PAGE_READONLY, &old2);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = r->ExceptionInformation[1], page = atomic_load(&watch_page), len = atomic_load(&watch_len);
    if (!len || a < page || a >= page + len) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect((void *)(uintptr_t)page, (SIZE_T)len, PAGE_READWRITE, &old);
    int keep = atomic_load(&watch_keep);
    if (keep) e->ContextRecord->EFlags |= 0x100, step_hit = -1;   // one instruction, then watch_veh again
    if (a < atomic_load(&watch_lo) || a >= atomic_load(&watch_hi)) {
        atomic_fetch_add(&watch_other, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    uint64_t rip = e->ContextRecord->Rip;
    int n = atomic_load(&watch_n);
    if (keep) step_old = *(const uint32_t *)(uintptr_t)atomic_load(&watch_lo);
    for (int i = 0; i < n; i++)
        if (watch_hits[i].rip == rip) {
            watch_hits[i].hits++;
            if (keep) step_hit = i;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    if (n < WATCH_MAX && atomic_compare_exchange_strong(&watch_n, &n, n + 1)) {
        WatchHit *h = &watch_hits[n];
        h->rip = rip, h->hits = 1, h->first_off = a - atomic_load(&watch_lo);
        if (keep) step_hit = n;
        Cpu *gc = kern_cpu();   // the guest's side: its link register, then the return addresses of its frame chain
        if (gc) {
            h->guest[0] = gc->x[30];
            uint64_t fp = gc->x[29];
            for (int g = 1; g < 6 && fp; g++) {
                uint64_t rec[2];
                if (!plat_mem_read(fp, rec, sizeof rec)) break;
                h->guest[g] = rec[1], fp = rec[0];
            }
        }
        const uint64_t *sp = (const uint64_t *)(uintptr_t)e->ContextRecord->Rsp;
        int k = 0;
        for (int i = 0; i < 400 && k < 6; i++)
            if (sp[i] >= image_lo && sp[i] < image_hi) h->stack[k++] = sp[i];
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_arm(uint64_t locals, uint64_t bytes) {
    static int installed;
    if (!installed) {
        installed = 1;
        HMODULE exe = GetModuleHandleW(NULL);
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)exe;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + dos->e_lfanew);
        image_lo = (uint64_t)(uintptr_t)exe, image_hi = image_lo + nt->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, watch_veh);
        rt_log("coop: puppet watch: totk.exe at %#llx", (unsigned long long)image_lo);
    }
    uint64_t page = locals & ~0xfffull, len = ((locals + bytes + 0xfff) & ~0xfffull) - page;
    atomic_store(&watch_lo, locals), atomic_store(&watch_hi, locals + bytes);
    atomic_store(&watch_page, page), atomic_store(&watch_len, len);
    DWORD old;
    VirtualProtect((void *)(uintptr_t)page, (SIZE_T)len, PAGE_READONLY, &old);
}

static void watch_report(void) {
    uint64_t page = atomic_load(&watch_page), len = atomic_load(&watch_len);
    DWORD old;
    atomic_store(&watch_len, 0);
    if (len) VirtualProtect((void *)(uintptr_t)page, (SIZE_T)len, PAGE_READWRITE, &old);
    int n = atomic_load(&watch_n);
    rt_log("coop: puppet watch: %d writers of the puppet's locals, %llu first writes elsewhere in its pages", n,
           (unsigned long long)atomic_load(&watch_other));
    for (int i = 0; i < n; i++) {
        const WatchHit *h = &watch_hits[i];
        rt_log("coop: puppet watch: exe+%#llx %llu times (%llu set the bit), first at +%#llx; stack exe+%#llx exe+%#llx exe+%#llx "
               "exe+%#llx exe+%#llx exe+%#llx", (unsigned long long)(h->rip - image_lo), (unsigned long long)h->hits,
               (unsigned long long)h->sets, (unsigned long long)h->first_off, (unsigned long long)(h->stack[0] ? h->stack[0] - image_lo : 0),
               (unsigned long long)(h->stack[1] ? h->stack[1] - image_lo : 0),
               (unsigned long long)(h->stack[2] ? h->stack[2] - image_lo : 0),
               (unsigned long long)(h->stack[3] ? h->stack[3] - image_lo : 0),
               (unsigned long long)(h->stack[4] ? h->stack[4] - image_lo : 0),
               (unsigned long long)(h->stack[5] ? h->stack[5] - image_lo : 0));
        rt_log("coop: puppet watch:     guest lr main+%#llx, frames main+%#llx main+%#llx main+%#llx main+%#llx main+%#llx",
               (unsigned long long)(h->guest[0] - MOD_MAIN_BASE), (unsigned long long)(h->guest[1] - MOD_MAIN_BASE),
               (unsigned long long)(h->guest[2] - MOD_MAIN_BASE), (unsigned long long)(h->guest[3] - MOD_MAIN_BASE),
               (unsigned long long)(h->guest[4] - MOD_MAIN_BASE), (unsigned long long)(h->guest[5] - MOD_MAIN_BASE));
    }
}
#else
static void watch_arm(uint64_t locals, uint64_t bytes) { (void)locals, (void)bytes; }
static void watch_report(void) {}
#endif

// TOTK_COOP_PUPPET_DIAG=1: once, 2 s after the puppet exists, the log gets both actors' class (vtable, as an offset
// in main) and their components (actor +0x228 is the component array, mouse_direct.c player_state): the slot and the
// class of each, to see what the player's actor has that the puppet lacks.
static void diag_actor(const char *who, uint64_t a) {
    uint64_t comps = read_ptr(a + 0x228);
    rt_log("coop: puppet diag: %s actor %#llx class main+%#llx components at %#llx", who, (unsigned long long)a,
           (unsigned long long)(read_ptr(a) - MOD_MAIN_BASE), (unsigned long long)comps);
    char line[900];
    int n = 0, count = 0;
    for (int i = 0; comps && i < 96; i++) {
        uint64_t p = read_ptr(comps + (uint64_t)i * 8), vt = p ? read_ptr(p) : 0;
        if (!p) continue;
        if (vt < MOD_MAIN_BASE || vt >= MOD_MAIN_BASE + MOD_MAIN_SIZE) continue;   // not an object of the game
        n += snprintf(line + n, sizeof line - (size_t)n, " %d:%llx", i, (unsigned long long)(vt - MOD_MAIN_BASE));
        if (++count % 14 == 0) {
            rt_log("coop: puppet diag: %s components%s", who, line);
            n = 0;
        }
    }
    if (n) rt_log("coop: puppet diag: %s components%s", who, line);
}

// The diag's search for an actor's bone matrices: arrays of at least 20 consecutive 3x4 float matrices whose
// translations lie within 3 m of the actor and whose first row has a length near 1, reached from one of the actor's
// components through at most DEPTH pointers. Logs each array once with the chain of offsets that led to it.
#define BFS_NODES 400000
#define BFS_DEPTH 5
typedef struct { uint64_t obj; int32_t parent; uint16_t off; uint8_t depth, comp; } BfsNode;

static int bone_run(uint64_t a, const float pos[3]) {
    float m[12 * 8];
    int n = 0;
    for (;; a += sizeof m) {
        if (!plat_mem_read(a, m, sizeof m)) return n;
        for (int i = 0; i < 8; i++) {
            const float *q = m + i * 12;
            float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
            float dx = q[3] - pos[0], dy = q[7] - pos[1], dz = q[11] - pos[2];
            if (!(len > 0.25f && len < 4.0f) || !(dx * dx + dy * dy + dz * dz < 9.0f)) return n;
            if (++n >= 600) return n;
        }
    }
}

// diag_found: the actor diag_bones found by name; diag_hits, diag_nhits: the distinct ones, in the order found

// Where an actor, its components and its model point at one of the known objects (tags): the links between a part
// and the body it belongs to.
__attribute__((unused)) static void diag_links(const char *who, uint64_t a, const uint64_t *tag, const char *const *tagname, int ntag) {
    uint64_t comps = read_ptr(a + 0x228);
    char line[900];
    for (int region = -2; region < 96; region++) {
        uint64_t base = region == -2 ? a : region == -1 ? read_ptr(read_ptr(comps + 8) + 0x28) : read_ptr(comps + (uint64_t)region * 8);
        uint64_t w[0x500 / 8];
        if (!base || !plat_mem_read(base, w, sizeof w)) continue;
        int n = 0;
        for (unsigned k = 0; k < sizeof w / 8 && n < (int)sizeof line - 40; k++)
            for (int t = 0; t < ntag; t++)
                if (tag[t] && w[k] == tag[t]) n += snprintf(line + n, sizeof line - (size_t)n, " +%#x=%s", k * 8, tagname[t]);
        if (n) rt_log("coop: puppet diag: links of %s, %s%d at %#llx:%s", who, region == -2 ? "actor " : region == -1 ? "model " : "component ",
                      region < 0 ? 0 : region, (unsigned long long)base, line);
    }
}

static void diag_bones(const char *who, uint64_t a, const Part *find) {
    float pos[3];
    uint64_t comps = read_ptr(a + 0x228);
    if (!comps || !plat_mem_read(a + 0x2b4, pos, sizeof pos)) return;
    BfsNode *q = calloc(BFS_NODES, sizeof *q);
    uint64_t *seen = calloc(1 << 19, sizeof *seen), found[24];
    if (!q || !seen) return;
    int head = 0, tail = 0, nfound = 0;
    for (int i = find ? 0 : 1; i < (find ? 96 : 2) && tail < BFS_NODES; i++) {   // component 1 holds the model
        uint64_t p = read_ptr(comps + (uint64_t)i * 8);
        if (p) q[tail++] = (BfsNode){p, -1, 0, 0, (uint8_t)i};
    }
    uint64_t words[0x400 / 8];
    while (head < tail && nfound < 24) {
        BfsNode nd = q[head];
        if (plat_mem_read(nd.obj, words, sizeof words)) {
            for (unsigned k = 0; !find && k + 15 < sizeof words / 8; k++) {   // an embedded skeleton: its class word
                if (words[k] != MOD_MAIN_BASE + 0x3909cd0) continue;
                uint64_t s = nd.obj + k * 8, h = (s * 0x9E3779B97F4A7C15ull) >> 45;
                int dup = 0;
                for (int probe = 0; probe < 8; probe++, h = (h + 1) & ((1 << 19) - 1)) {
                    if (seen[h] == s) { dup = 1; break; }
                    if (!seen[h]) { seen[h] = s; break; }
                }
                if (dup) continue;
                char chain[200];
                int n = snprintf(chain, sizeof chain, "+%#x", k * 8);
                for (int at = head; at >= 0 && q[at].parent >= 0 && n < (int)sizeof chain - 16; at = q[at].parent)
                    n += snprintf(chain + n, sizeof chain - (size_t)n, " <+%#x", q[at].off);
                rt_log("coop: puppet diag: %s skeleton at %#llx: %u bones, locals %#llx, +0x58 %#llx, +0x68 %#llx, +0x70 "
                       "%#llx; chain (last first) %s", who, (unsigned long long)s, (unsigned)(words[k + 12] >> 32) & 0xffff,
                       (unsigned long long)words[k + 9], (unsigned long long)words[k + 11], (unsigned long long)words[k + 13],
                       (unsigned long long)words[k + 14], chain);
                if (++nfound >= 24) break;
            }
            for (unsigned k = 0; k < sizeof words / 8; k++) {
                uint64_t p = words[k];
                if (p < 0x100000000ull || p >= 0x7000000000ull || (p & 3)) continue;   // heap pointers only
                uint64_t h = (p * 0x9E3779B97F4A7C15ull) >> 45;
                int dup = 0;
                for (int probe = 0; probe < 8; probe++, h = (h + 1) & ((1 << 19) - 1)) {
                    if (seen[h] == p) { dup = 1; break; }
                    if (!seen[h]) { seen[h] = p; break; }
                }
                if (dup) continue;
                if (p == 0) continue;
                int run = find && p != a && part_named(find, p) ? 20 : 0;
                if (run >= 20) {
                    char chain[200];
                    int n = snprintf(chain, sizeof chain, "+%#x", k * 8);
                    for (int at = head; at >= 0 && q[at].parent >= 0 && n < (int)sizeof chain - 16; at = q[at].parent)
                        n += snprintf(chain + n, sizeof chain - (size_t)n, " <+%#x", q[at].off);
                    rt_log("coop: puppet diag: %s bones: %d matrices at %#llx, held at %#llx; component %d, chain (last "
                           "first) %s", who, run, (unsigned long long)p, (unsigned long long)(nd.obj + k * 8), nd.comp,
                           chain);
                    found[nfound++] = p;
                    if (find && p != find->actor) {
                        diag_found = p;
                        int known = 0;
                        for (int h = 0; h < diag_nhits; h++) known |= diag_hits[h] == p;
                        if (!known && diag_nhits < 4) diag_hits[diag_nhits++] = p;
                    }
                    if (nfound >= 24) break;
                } else if (nd.depth < BFS_DEPTH && tail < BFS_NODES) {
                    q[tail++] = (BfsNode){p, head, (uint16_t)(k * 8), (uint8_t)(nd.depth + 1), nd.comp};
                }
            }
        }
        head++;
    }
    rt_log("coop: puppet diag: %s bones: searched %d objects, %d arrays", who, head, nfound);
    (void)found;
    free(q), free(seen);
}

// The object the search found the puppet's matrices in: [[component c + 0x2f8] + 0x58], for components 1 and 2 (the
// two of one class). Logs its words, and for each pointer in it how many bone matrices it leads to.
static void diag_skel(const char *who, uint64_t a) {
    float pos[3];
    uint64_t comps = read_ptr(a + 0x228);
    if (!comps || !plat_mem_read(a + 0x2b4, pos, sizeof pos)) return;
    for (int ci = 1; ci <= 2; ci++) {
        uint64_t comp = read_ptr(comps + (uint64_t)ci * 8), o = read_ptr(comp + 0x2f8), x = read_ptr(o + 0x58);
        rt_log("coop: puppet diag: %s component %d %#llx: +0x2f8 %#llx (class %llx), its +0x58 %#llx (class %llx)", who, ci,
               (unsigned long long)comp, (unsigned long long)o, (unsigned long long)(read_ptr(o) - MOD_MAIN_BASE),
               (unsigned long long)x, (unsigned long long)(read_ptr(x) - MOD_MAIN_BASE));
        uint64_t w[0x180 / 8];
        if (!x || !plat_mem_read(x, w, sizeof w)) continue;
        char line[1000];
        int n = 0;
        for (unsigned k = 0; k < sizeof w / 8; k++) {
            int run = w[k] >= 0x100000000ull && w[k] < 0x7000000000ull ? bone_run(w[k], pos) : 0;
            n += snprintf(line + n, sizeof line - (size_t)n, " %x:%llx", k * 8, (unsigned long long)w[k]);
            if (run) n += snprintf(line + n, sizeof line - (size_t)n, "(%d mats)", run);
            if (k % 8 == 7) {
                rt_log("coop: puppet diag: %s c%d skel%s", who, ci, line);
                n = 0;
            }
        }
        static const unsigned at[] = {0x80, 0x88, 0x130, 0x138};
        for (unsigned j = 0; j < 4 && ci == 1; j++) {
            float f[36];
            if (!plat_mem_read(w[at[j] / 8], f, sizeof f)) continue;
            n = 0;
            for (int k = 0; k < 36; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %.3f", f[k]);
            rt_log("coop: puppet diag: %s floats at [+%#x] (actor at %.1f %.1f %.1f):%s", who, at[j], pos[0], pos[1],
                   pos[2], line);
        }
    }
}

// ---- TOTK_COOP_TRACE=1 (the environment, read at start): logs every call of the game's functions that create
// actors and equipment, with their arguments and the names they point at, through the function-entry hook of the
// lifted code (cpu.h FN_ENTER, main.c fnprof_hit). The hook runs on every guest call while it is on, so the game is
// slower for the length of the trace: it ends 12 s after the world loaded.
#include <stdlib.h>
static int trace_on;
static _Atomic int trace_lines;

static int printable(uint64_t a, char out[48]) {
    char s[48];
    if (a < 0x10000 || !plat_mem_read(a, s, sizeof s)) return 0;
    int n = 0;
    while (n < 47 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (n < 3 || s[n]) return 0;
    memcpy(out, s, (size_t)n + 1);
    return 1;
}

void coop_trace_hit(uint64_t addr, uint64_t lr) {
    if (!trace_on) return;
    static const uint64_t fns[] = {0x599670, 0xa1bc30, 0xaae1f8, 0xc4bf78, 0xaaa240, 0xd2029c, 0x220cb24, 0xc45cd8};
    static const char *const names[] = {"createAsync", "createSync", "connectSyncImpl", "connectSync", "equipCreate(0xaaa240)",
                                        "riderCreate(0xd2029c)", "0x220cb24", "0xc45cd8"};
    int which = -1;
    for (unsigned i = 0; i < sizeof fns / sizeof *fns; i++)
        if (addr == MOD_MAIN_BASE + fns[i]) which = (int)i;
    if (which < 0) return;
    Cpu *c = kern_cpu();
    char line[1200], s[48];
    if (!c) return;
    if (which == 0) {   // of the many plain requests, those of the player's kind only
        if (!printable(read_ptr(c->x[1]), s) || (strncmp(s, "Armor_", 6) && !strstr(s, "Pouch") && !strstr(s, "Player") &&
                                                 strncmp(s, "Weapon_", 7)))
            return;
    }
    if (atomic_fetch_add(&trace_lines, 1) > 900) return;
    int n = snprintf(line, sizeof line, "coop: trace: %s from main+%#llx:", names[which], (unsigned long long)(lr - MOD_MAIN_BASE));
    for (int i = 0; i < 8; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " x%d=%llx", i, (unsigned long long)c->x[i]);
        if (printable(c->x[i], s) || printable(read_ptr(c->x[i]), s)) n += snprintf(line + n, sizeof line - (size_t)n, "\"%s\"", s);
        else if (printable(read_ptr(c->x[i] + ACTOR_NAME), s)) n += snprintf(line + n, sizeof line - (size_t)n, "<actor %s>", s);
    }
    if (which <= 3) {   // the CreateArg at x2: its actor fields
        static const char *const fld[] = {"parent", "dependent", "creator"};
        for (int i = 0; i < 3; i++) {
            uint64_t p = read_ptr(c->x[2] + 0x68 + (uint64_t)i * 8);
            if (!p) continue;
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llx", fld[i], (unsigned long long)p);
            if (printable(read_ptr(p + ACTOR_NAME), s)) n += snprintf(line + n, sizeof line - (size_t)n, "<actor %s>", s);
        }
        uint64_t owner = read_ptr(c->x[2] + 0x68), ocomps = read_ptr(owner + 0x228), best = 0;
        int besti = -1;
        for (int i = 0; ocomps && c->x[3] && i < 96; i++) {   // the watcher slot (x3) as an offset in one of the parent's components
            uint64_t p = read_ptr(ocomps + (uint64_t)i * 8);
            if (p && p <= c->x[3] && p > best) best = p, besti = i;
        }
        if (besti >= 0) n += snprintf(line + n, sizeof line - (size_t)n, " slot=component %d +%#llx (class %llx)", besti,
                                      (unsigned long long)(c->x[3] - best), (unsigned long long)(read_ptr(best) - MOD_MAIN_BASE));
        uint64_t slotw[3] = {0};
        plat_mem_read(c->x[3], slotw, sizeof slotw);
        uint64_t fn7[2] = {0};
        plat_mem_read(c->x[7], fn7, sizeof fn7);
        n += snprintf(line + n, sizeof line - (size_t)n, " slot words %llx %llx %llx; callback class %llx", (unsigned long long)slotw[0],
                      (unsigned long long)slotw[1], (unsigned long long)slotw[2], (unsigned long long)(fn7[0] - MOD_MAIN_BASE));
        uint64_t cb[8] = {0};   // the callback object's words, named where they are the owner, its equipment component or the slot
        plat_mem_read(c->x[7], cb, sizeof cb);
        n += snprintf(line + n, sizeof line - (size_t)n, " callback:");
        for (int i = 0; i < 8; i++) {
            const char *is = cb[i] && cb[i] == owner ? "=OWNER" : cb[i] && cb[i] == best ? "=EQUIP" : cb[i] && cb[i] == c->x[3] ? "=SLOT"
                           : cb[i] >= MOD_MAIN_BASE && cb[i] < MOD_MAIN_BASE + MOD_MAIN_SIZE ? "(main)"
                           : best && cb[i] > best && cb[i] < best + 0x2000 ? "(in EQUIP)" : cb[i] > c->sp - 0x4000 && cb[i] < c->sp + 0x4000 ? "(stack)" : "";
            n += snprintf(line + n, sizeof line - (size_t)n, " %llx%s", (unsigned long long)(cb[i] >= MOD_MAIN_BASE && cb[i] < MOD_MAIN_BASE + MOD_MAIN_SIZE ? cb[i] - MOD_MAIN_BASE : cb[i]), is);
        }
        n += snprintf(line + n, sizeof line - (size_t)n, " callers:");   // the frame chain: [x29] the caller's frame, [x29+8] its return address
        for (uint64_t fp = c->x[29], depth = 0; fp && depth < 7; depth++) {
            uint64_t rec[2];
            if (!plat_mem_read(fp, rec, sizeof rec)) break;
            n += snprintf(line + n, sizeof line - (size_t)n, " %llx", (unsigned long long)(rec[1] - MOD_MAIN_BASE));
            fp = rec[0];
        }
        uint64_t bb = read_ptr(c->x[2] + 0x58);
        uint8_t fl = 0;
        plat_mem_read(c->x[2] + 0x90, &fl, 1);
        n += snprintf(line + n, sizeof line - (size_t)n, " blackboard=%llx flags=%u", (unsigned long long)bb, fl);
        uint32_t ca[0x98 / 4] = {0};   // the whole CreateArg and the call's stack arguments, to compare two calls word by word
        uint64_t st[4] = {0};
        plat_mem_read(c->x[2], ca, sizeof ca), plat_mem_read(c->sp, st, sizeof st);
        char raw[700];
        int r = snprintf(raw, sizeof raw, "coop: trace:   arg");
        for (unsigned i = 0; i < sizeof ca / 4; i++) r += snprintf(raw + r, sizeof raw - (size_t)r, " %x", ca[i]);
        r += snprintf(raw + r, sizeof raw - (size_t)r, " stack %llx %llx %llx %llx", (unsigned long long)st[0], (unsigned long long)st[1],
                      (unsigned long long)st[2], (unsigned long long)st[3]);
        rt_log("%s", line);
        rt_log("%s", raw);
        return;
    }
    rt_log("%s", line);
}

__attribute__((constructor)) static void trace_start(void) {
    const char *e = getenv("TOTK_COOP_TRACE");
    if (e && e[0] == '1') trace_on = 1, g_fnprof = 1;
}

// frame_rate.c, at each present while the link runs: keeps the hook on (resolution.c switches it off when it has what
// it waits for) until the trace ends.
void coop_trace_present(void) {
    if (!trace_on) return;
    int64_t w = frame_step_world_us();
    if (w > 12000000) {
        trace_on = 0, g_fnprof = 0;
        rt_log("coop: trace: ended, %d calls seen", atomic_load(&trace_lines));
    } else {
        g_fnprof = 1;
    }
}
// frame_rate.c frame_step_present, on the presenting guest thread. self: this side's player position; player: its
// actor (0 when not known).
void coop_puppet_present(const float self[3], uint64_t player) {
    if (state < 0 || mode() <= 0) return;
    Cpu *c = kern_cpu();
    if (!c) return;
    static uint64_t sent_ns;
    uint64_t t = plat_now_ns();
    if (player && t - sent_ns >= 33000000ull) {
        static int sent = -1;
        int ok = pose_publish(player);
        sent_ns = t;
        if (ok != sent) {
            sent = ok;
            rt_log("coop: puppet: this side's pose is %s", ok ? "sent to the peer" : "not sent: a skeleton of the player "
                                                                                    "was not found");
        }
    }
    equip_publish(player);
    CoopPose r;
    memset(&r, 0, sizeof r);
    uint64_t now = plat_now_ns();
    int have = coop_read_remote(&r, REMOTE_MAX_AGE_NS);
    float pos[3] = {r.pos[0], r.pos[1], r.pos[2]};
    if (have && mode() == 2) pos[0] += 2.0f;   // the test with coop_peer = loop: beside this side's player, not inside it
    float dx = pos[0] - self[0], dy = pos[1] - self[1], dz = pos[2] - self[2];
    if (!have || dx * dx + dy * dy + dz * dz > SPAWN_RANGE_M * SPAWN_RANGE_M) {
        // No peer for HIDE_AFTER_NS (it left, its game is loading, the link broke) or the peer out of range: a puppet
        // that exists is put HIDE_UP_M above this side's player, where nothing is drawn near the camera, instead of
        // standing frozen where the peer was last seen. It comes back with the peer's next pose in range.
        static uint64_t lost_ns;
        if (have) lost_ns = 0;
        else if (!lost_ns) lost_ns = now;
        if (state == 2 && (have || now - lost_ns > HIDE_AFTER_NS) && named(actor)) {
            float away[3] = {self[0], self[1] + HIDE_UP_M, self[2]}, m[12];
            matrix_of(away, 0, m);
            rt_call(c, FN_FORCE, 3, actor, (uint64_t)(uintptr_t)m, (uint64_t)0);
            static int said;
            if (!said) said = 1, rt_log("coop: puppet: no peer in range; the puppet is put out of sight");
        }
        return;
    }
    if (state == 0) {
        if (now >= retry_ns) request(c, pos, r.yaw);
        return;
    }
    if (state == 1) {
        static uint64_t asked_ns;
        if (!asked_ns) asked_ns = now;
        actor = created();
        if (actor) {
            state = 2, asked_ns = 0;
            unpause_note(actor);
            rt_log("coop: puppet: the game created %s, actor %#llx", name_buf, (unsigned long long)actor);
        } else if (now - asked_ns > 2 * RETRY_NS) {
            rt_log("coop: puppet: no actor after 10 s (PreActor %#llx); asking again", (unsigned long long)preactor);
            state = 0, asked_ns = 0, retry_ns = now + RETRY_NS;
            return;
        } else {
            return;
        }
    }
    if (!named(actor)) {   // deleted by a load
        rt_log("coop: puppet: the actor is gone; asking again in 5 s");
        pose_hook(0);
        unpause_note(0);
        state = 0, actor = 0, retry_ns = now + RETRY_NS;
        return;
    }
    int32_t proc_state = 0;
    if (plat_mem_read(actor + 0x24, &proc_state, 4) && proc_state >= 7 && proc_state <= 11) return;   // being deleted
    float m[12];
    static int16_t bones[COOP_BONE_VALUES];
    int have_bones = coop_read_remote_bones(bones, REMOTE_MAX_AGE_NS);
    matrix_of_rot(pos, r.yaw, have_bones ? bones + COOP_BONES * COOP_BONE_FIELDS : NULL, m);
    rt_call(c, FN_FORCE, 3, actor, (uint64_t)(uintptr_t)m, (uint64_t)0);
    {   // experiment (TOTK_COOP_PUPPET_DIAG=h6): the puppet's pause bit 0x8000, which the player's actor does not have
        const char *d6 = cfg_get("TOTK_COOP_PUPPET_DIAG");
        uint32_t pf = 0;
        if (d6 && d6[0] == 'h' && d6[1] == '6' && plat_mem_read(actor + 0x5ac, &pf, 4) && (pf & 0x8000))
            *(uint32_t *)(uintptr_t)(actor + 0x5ac) = pf & ~0x8000u;
    }
    int hair_mode = cfg_int("TOTK_COOP_PUPPET_HAIR");
    if (hair_mode > 0) {
        // The puppet's equipment component is its component 70 (class main+0x3672c18), as the player's is. When the
        // game makes a PouchPlayer for the inventory screen, that component's start (main+0xc45cd8) dresses it by
        // calling main+0x1b6cd78 (component, slot number, a SafeString with the piece's name, -1, 0, 0) per slot:
        // slot 0 with an empty name takes the player's weapon, 15 and 16 are the two hair pieces
        // (Armor_Default_Extra_00 and _01, the names at main+0x3672320 and +0x3672328), 18 Accessory_Battery
        // (main+0x3672338), 19 the arm (PlayerRaulHand, main+0x3672340), 9, 10 and 11 the head, upper and lower armour. A PouchPlayer made outside
        // the inventory screen is not dressed by the game, so the same calls are made here, once per puppet.
        //
        // Through that function (coop_puppet_hair 1 and 2) the pieces are NOT drawn in the world: the game then requests the pieces as it does for its
        // own PouchPlayer (TOTK_COOP_TRACE shows the same callers, slots and callbacks), and the puppet's body hides
        // what a piece replaces (with the lower armour its legs are gone), but no piece is drawn in the world. Requesting
        // a piece directly (part_present: the puppet as parent and creator, its slot as the watcher, the equipment
        // callback main+0x36727e0) creates it and shows nothing either. Open: where the pieces are drawn or placed;
        // the inventory screen draws its PouchPlayer in a view of its own.
        //
        // What the diags found since (TOTK_COOP_PUPPET_DIAG=h, hq, p): the puppet's hair actor exists, sits at the
        // puppet's position and its model matrix follows, so it is placed right and only not drawn. Against the
        // player's own hair actor it differs in the pointers at actor +0x140..+0x178 (the player's hair has the first
        // three and +0x168, the puppet's has none of them; the puppet's body lacks the first two), in the pause word
        // +0x5ac (bit 1 set on the puppet's hair, set once and not again after it is cleared; bit 0x8000 set on the
        // puppet's body by main+0x426d74 in the actor's own update every frame) and in model words (+0x240
        // 0x16810/0x10010, +0x274 bit 0x400000, +0x2ac and +0x324 1/0). Writing the player's values over any of these
        // at the present changes nothing on screen. Asking for a second actor named Player instead of PouchPlayer
        // crashes the game (a null actor, reading +0x228). SOLVED by coop_puppet_hair = 3 (the default) below: the
        // player's own piece function, whose pieces are drawn in the world (TOTK_COOP_TRACE: its CreateArg has no
        // blackboard, the pouch path's has one).
        static uint64_t dressed, known_for, known_since;
        static const uint32_t dress[4] = {0xd10383ff, 0xa9087bfd, 0xa9096ffc, 0xa90a67fa};
        static const uint32_t piece[4] = {0xd10303ff, 0xa9067bfd, 0xa9076ffc, 0xa90867fa};   // FN_PIECE's first words
        if (known_for != actor) known_for = actor, known_since = now;
        uint64_t equip = read_ptr(read_ptr(actor + 0x228) + 70 * 8);
        atomic_store_explicit(&puppet_equip, equip, memory_order_relaxed);
        if (dressed != actor && equip && read_ptr(equip) == MOD_MAIN_BASE + 0x3672c18 && words_are(FN_DRESS, dress) &&
            now - known_since > 1000000000ull) {
            dressed = actor;
            static const char *empty = "", *head = "Armor_999_Head", *upper = "Armor_Default_Upper", *lower = "Armor_Default_Lower";
            static const struct { int slot; uint64_t name; int from; } slots[] = {
                {0, 0, 2}, {9, 0, 2}, {10, 0, 2}, {11, 0, 2},   // the game's order: weapon, armour, then the fixed pieces
                {15, MOD_MAIN_BASE + 0x3672320, 1}, {16, MOD_MAIN_BASE + 0x3672328, 1}, {19, MOD_MAIN_BASE + 0x3672340, 2},
            };
            // 3: the player's own path instead, main+0xc48ac0 (component, slot, name, -1, 0, 1), as the equipment
            // component's start calls it for the player at main+0xc470dc; the pouch path above marks pieces for the
            // inventory screen's view only (its CreateArg carries a blackboard the player's does not).
            if (hair_mode == 3 && words_are(FN_PIECE, piece)) {
                // the player's start order: head, upper and lower armour (the default names at main+0x3672350, +0x58,
                // +0x60), the two hair pieces, the Zonai arm
                static const int ps[] = {9, 10, 11, 15, 16, 19};
                static const uint64_t pn[] = {MOD_MAIN_BASE + 0x3672350, MOD_MAIN_BASE + 0x3672358, MOD_MAIN_BASE + 0x3672360,
                                              MOD_MAIN_BASE + 0x3672320, MOD_MAIN_BASE + 0x3672328, MOD_MAIN_BASE + 0x3672340};
                for (int i = 0; i < 6; i++) {
                    uint64_t ok = rt_call(c, FN_PIECE, 6, equip, (uint64_t)ps[i], pn[i], (uint64_t)0xffffffffull, (uint64_t)0, (uint64_t)1);
                    rt_log("coop: puppet: asked the game for the player's piece of slot %d on the puppet: %llu", ps[i],
                           (unsigned long long)(ok & 0xff));
                }
            }
            for (unsigned i = 0; hair_mode != 3 && i < sizeof slots / sizeof *slots; i++) {
                if (slots[i].from > hair_mode) continue;
                uint64_t name = slots[i].name ? slots[i].name
                              : (uint64_t)(uintptr_t)(slots[i].slot == 9 ? &head : slots[i].slot == 10 ? &upper : slots[i].slot == 11 ? &lower : &empty);
                rt_call(c, FN_DRESS, 6, equip, (uint64_t)slots[i].slot, name, (uint64_t)0xffffffffull, (uint64_t)0, (uint64_t)0);
                rt_log("coop: puppet: asked the game to dress slot %d of the puppet", slots[i].slot);
            }
        }
        // What the peer wears (its table from the link, coop_puppet.c host_piece_note on its side): each slot whose
        // piece differs from what the puppet has gets the peer's piece through the same function, with the peer's
        // arguments. Once the defaults above are on, and again whenever the peer's table changes.
        static uint64_t worn_for;
        static uint32_t worn_gen;
        static char worn[COOP_EQUIP_SLOTS][COOP_EQUIP_NAME];
        static const char *worn_ref[COOP_EQUIP_SLOTS];   // what x2 points at; the game copies the name
        static CoopEquip pe;
        // Taking a piece off: the game's piece functions only put pieces on (an empty name changes nothing), so a
        // piece the peer no longer has is hidden instead. Right after a piece is asked for, its slot's request in the
        // component (+0x1430 + slot * 8) leads to the piece's actor once the game made it, as the puppet's own
        // PreActor does (created). A hidden piece gets bit 0x8 in its model's word +0x240 at every present, the bit the
        // draw pass skips a model on (main+0x8e53c); tried on the quiver, which then vanishes while the bow stays.
        static uint64_t piece_pre[COOP_EQUIP_SLOTS], piece_actor[COOP_EQUIP_SLOTS], piece_since[COOP_EQUIP_SLOTS];
        static char piece_name[COOP_EQUIP_SLOTS][COOP_EQUIP_NAME];
        static uint8_t piece_hidden[COOP_EQUIP_SLOTS];
        if (worn_for != actor) {
            worn_for = actor, worn_gen = 0;
            memset(worn, 0, sizeof worn);
            memset(piece_pre, 0, sizeof piece_pre), memset(piece_actor, 0, sizeof piece_actor), memset(piece_since, 0, sizeof piece_since);
            memset(piece_hidden, 0, sizeof piece_hidden);
            static const struct { int slot; const char *name; } defaults[] = {
                {9, "Armor_999_Head"}, {10, "Armor_Default_Upper"}, {11, "Armor_Default_Lower"},
                {15, "Armor_Default_Extra_00"}, {16, "Armor_Default_Extra_01"}, {19, "PlayerRaulHand"}};
            for (unsigned i = 0; i < sizeof defaults / sizeof *defaults; i++)
                snprintf(worn[defaults[i].slot], COOP_EQUIP_NAME, "%s", defaults[i].name);
        }
        if (hair_mode == 3 && dressed == actor && equip && now - known_since > 1500000000ull && coop_read_remote_equip(&pe) &&
            pe.gen != worn_gen && words_are(FN_PIECE, piece)) {
            worn_gen = pe.gen;
            // The game shows the quiver (slot 6) only while a bow is equipped: the player's table always has one (it is
            // made at every load), so the puppet gets it only when the peer has a bow, and loses it with the bow.
            // The bow is in slot 7, the shield in 8, the sword or other one-handed weapon in 0.
            int bow = 0;
            for (int s = 0; s < COOP_EQUIP_SLOTS; s++)
                if ((pe.flags[s] & COOP_EQUIP_NOTED) && !strncmp(pe.name[s], "Weapon_Bow_", 11)) bow = 1;
            if (!bow && (pe.flags[6] & COOP_EQUIP_NOTED)) pe.name[6][0] = 0;
            // the same for the sheath (slot 3) and the weapon in slot 0
            if (!pe.name[0][0] && (pe.flags[3] & COOP_EQUIP_NOTED)) pe.name[3][0] = 0;
            for (int s = 0; s < COOP_EQUIP_SLOTS; s++) {
                if (!(pe.flags[s] & COOP_EQUIP_NOTED) || !strcmp(worn[s], pe.name[s])) continue;
                memcpy(worn[s], pe.name[s], COOP_EQUIP_NAME);
                worn_ref[s] = worn[s];
                if (!worn[s][0]) {   // taken off: hide what the slot holds
                    piece_hidden[s] = 1;
                    rt_log("coop: equip: the puppet's piece in slot %d is hidden (%s)", s, piece_actor[s] ? "found" : "not found yet");
                    continue;
                }
                if (piece_hidden[s] && piece_actor[s] && piece_named(piece_actor[s], piece_name[s])) {
                    uint64_t m = piece_model(piece_actor[s]);   // shown again: the game may reuse it
                    if (m) *(volatile uint32_t *)(uintptr_t)(m + 0x240) &= ~0x8u;
                }
                piece_hidden[s] = 0;
                uint64_t ok;
                puppet_asked_mask = 0;
                static const uint32_t weapon_words[4] = {0xa9ba7bfd, 0xa9016ffc, 0x910003fd, 0xa90267fa};
                const char *wn = worn[s];
                int weapon = !strncmp(wn, "Weapon_", 7) && strncmp(wn, "Weapon_Sheath_", 14) && strncmp(wn, "Weapon_Quiver_", 14);
                if (weapon && (pe.flags[s] & COOP_EQUIP_WEAPON) && words_are(FN_WEAPON, weapon_words)) {
                    // the game's weapon function, as the player's equip-by-name calls it (main+0xc4ae90): x3 2, x4 and
                    // x5 0, x6 two words the game fills from a lookup (zero here), x7 1, and six stack words
                    static uint64_t six[2];
                    uint64_t sp = c->sp, frame = (sp - 0x40) & ~15ull;
                    uint64_t *st = (uint64_t *)(uintptr_t)frame;
                    st[0] = 0, st[1] = 0xffffffffu, st[2] = 0, st[3] = 0xffffffffu, st[4] = 0, st[5] = 0;
                    six[0] = six[1] = read_ptr(MOD_MAIN_BASE + 0x395db78);   // what the caller puts there: the game's "" (main+0xc4a878)
                    c->sp = frame;
                    uint32_t a = pe.w3[s];   // the peer's x3, x4 and x5 (host_weapon_note)
                    ok = rt_call(c, FN_WEAPON, 8, equip, (uint64_t)s, (uint64_t)(uintptr_t)&worn_ref[s], (uint64_t)(a & 0xff),
                                 (uint64_t)(a >> 8 & 0xffff), (uint64_t)(a >> 24), (uint64_t)(uintptr_t)six, (uint64_t)1);
                    c->sp = sp;
                } else {
                    ok = rt_call(c, FN_PIECE, 6, equip, (uint64_t)s, (uint64_t)(uintptr_t)&worn_ref[s], (uint64_t)pe.w3[s],
                                 (uint64_t)(pe.flags[s] & COOP_EQUIP_W4 ? 1 : 0), (uint64_t)(pe.flags[s] & COOP_EQUIP_W5 ? 1 : 0));
                }
                piece_pre[s] = read_ptr(equip + 0x1430 + (uint64_t)s * 8), piece_actor[s] = 0, piece_since[s] = now;
                memcpy(piece_name[s], worn[s], COOP_EQUIP_NAME);
                for (int o = 0; o < COOP_EQUIP_SLOTS; o++) {   // pieces the game put on inside that call, in other slots
                    if (o == s || !(puppet_asked_mask & 1u << o) || !puppet_asked[o][0]) continue;
                    piece_pre[o] = read_ptr(equip + 0x1430 + (uint64_t)o * 8), piece_actor[o] = 0, piece_since[o] = now;
                    piece_hidden[o] = 0;
                    memcpy(piece_name[o], puppet_asked[o], COOP_EQUIP_NAME);
                    memcpy(worn[o], puppet_asked[o], COOP_EQUIP_NAME);
                    rt_log("coop: equip: with it the game put %s in slot %d", puppet_asked[o], o);
                }
                puppet_asked_mask = 0;
                rt_log("coop: equip: the puppet now wears %s in slot %d (the game says %llu, request %#llx)", worn[s], s,
                       (unsigned long long)(ok & 0xff), (unsigned long long)piece_pre[s]);
            }
        }
        for (int o = 0; o < COOP_EQUIP_SLOTS && puppet_asked_mask; o++) {
            // pieces the game asked for the puppet on its own since (a weapon's own sheath comes some frames after the
            // weapon): followed like the ones asked here, hidden too when the slot is to be hidden
            if (!(puppet_asked_mask & 1u << o) || !puppet_asked[o][0] || !strcmp(piece_name[o], puppet_asked[o])) continue;
            piece_pre[o] = read_ptr(equip + 0x1430 + (uint64_t)o * 8), piece_actor[o] = 0, piece_since[o] = now;
            memcpy(piece_name[o], puppet_asked[o], COOP_EQUIP_NAME);
            rt_log("coop: equip: the game put %s in slot %d of the puppet", puppet_asked[o], o);
        }
        puppet_asked_mask = 0;
        for (int s = 0; s < COOP_EQUIP_SLOTS; s++) {
            if (piece_since[s] && !piece_actor[s]) {   // the piece's actor, once the game made it (10 s at most)
                uint64_t words[0x200 / 8], cur = read_ptr(equip + 0x1430 + (uint64_t)s * 8);
                if (cur && cur != piece_pre[s]) piece_pre[s] = cur;   // the request may be stored after the call
                if (now - piece_since[s] > 10000000000ull) {
                    piece_since[s] = 0, piece_pre[s] = 0;
                    continue;
                }
                if (!piece_pre[s] || !plat_mem_read(piece_pre[s], words, sizeof words)) continue;
                piece_actor[s] = find_named(words, sizeof words / 8, piece_pre[s], piece_name[s]);
                if (piece_actor[s]) {
                    piece_pre[s] = 0, piece_since[s] = 0;
                    rt_log("coop: equip: the puppet's %s is actor %#llx", piece_name[s], (unsigned long long)piece_actor[s]);
                }
            }
            if (piece_hidden[s] && piece_actor[s] && piece_named(piece_actor[s], piece_name[s])) {
                uint64_t m = piece_model(piece_actor[s]);
                if (m) *(volatile uint32_t *)(uintptr_t)(m + 0x240) |= 0x8u;
            }
        }
    }
    if (have_bones) {
        static int hooked = -1;
        static uint64_t said_ns;
        pose_store(bones);
        int n = pose_hook(actor);
        if (n != hooked) {
            hooked = n;
            said_ns = now;
            rt_log("coop: puppet: the peer's pose goes into %d of the puppet's 2 skeletons", n);
        }
        if (said_ns && now - said_ns > 3000000000ull) {   // once: is the pose reaching the game's animation step?
            said_ns = 0;
            rt_log("coop: puppet: the pose was put in %u times in the first 3 s",
                   atomic_load_explicit(&calc_calls, memory_order_relaxed));
        }
    }
#ifdef RT_PATCH_PAGE
    {   // TOTK_COOP_PUPPET_DIAG=u: every 4 s the puppet's place, its model's place, its words and the unpause count
        const char *du = cfg_get("TOTK_COOP_PUPPET_DIAG");
        static uint64_t next_u;
        static int nu;
        if (du && strchr(du, 'u') && nu < 8 && now >= next_u) {
            next_u = now + 4000000000ull, nu++;
            float ap[3] = {0}, mm[12] = {0};
            uint32_t f5ac = 0;
            int32_t st = 0;
            plat_mem_read(actor + 0x2b4, ap, sizeof ap), plat_mem_read(actor + 0x5ac, &f5ac, 4), plat_mem_read(actor + 0x24, &st, 4);
            uint64_t model = read_ptr(read_ptr(read_ptr(actor + 0x228) + 8) + 0x28);
            if (model) plat_mem_read(model + 0x1f8, mm, sizeof mm);
            rt_log("coop: puppet diag: actor %.1f %.1f %.1f, model %.1f %.1f %.1f, +0x5ac %08x, state %d, forced %u, "
                   "wanted %.1f %.1f %.1f", ap[0], ap[1], ap[2], mm[3], mm[7], mm[11], f5ac, st,
                   atomic_load_explicit(&flag_forced, memory_order_relaxed), m[3], m[7], m[11]);
        }
    }
#endif
    static uint64_t known_ns;
    static int diag_done, num_done;
    if (!known_ns) known_ns = now;
    const char *dn = cfg_get("TOTK_COOP_PUPPET_DIAG");
    if (dn && dn[0] == 'e' && player) {   // the equipment slots of both, 6 s after the puppet exists
        static int eq_done;
        if (!eq_done && now - known_ns > 6000000000ull) {
            eq_done = 1;
            for (int who = 0; who < 2; who++) {
                uint64_t a = who ? actor : player, eq = read_ptr(read_ptr(a + 0x228) + 70 * 8);
                {   // the component's words that differ between the player's and the puppet's (no pointers)
                    static uint32_t cw[2][0x4800 / 4];
                    memset(cw[who], 0, sizeof cw[who]);
                    for (unsigned o = 0; o < sizeof cw[who]; o += 0x100) plat_mem_read(eq + o, (uint8_t *)cw[who] + o, 0x100);
                    if (who) {
                        char line[900];
                        int n = 0;
                        for (unsigned k = 0; k < 0x4800 / 4; k++) {
                            uint32_t x = cw[0][k], y = cw[1][k];
                            if (x == y) continue;
                            uint32_t hx = k + 1 < 0x4800 / 4 ? cw[0][k + 1] : 0;
                            if (!(k & 1) && (hx == 1 || (hx >= 0x70 && hx <= 0x7fff)) ) { k++; continue; }   // a pointer
                            n += snprintf(line + n, sizeof line - (size_t)n, " +%#x:%x/%x", k * 4, x, y);
                            if (n > 800) { rt_log("coop: puppet diag: equip words (player/puppet):%s", line); n = 0; }
                        }
                        if (n) rt_log("coop: puppet diag: equip words (player/puppet):%s", line);
                    }
                }
                static uint8_t buf[0x4000];
                memset(buf, 0, sizeof buf);
                plat_mem_read(eq, buf, sizeof buf);
                for (unsigned o = 0; o + 8 < sizeof buf; o++) {   // every name of a piece the component holds
                    if ((buf[o] == 'A' && !memcmp(buf + o, "Armor_", 6)) || (buf[o] == 'W' && !memcmp(buf + o, "Weapon_", 7)) ||
                        (buf[o] == 'P' && !memcmp(buf + o, "PlayerRaul", 10))) {
                        if (o && buf[o - 1] >= 'A' && buf[o - 1] <= 'z') continue;
                        rt_log("coop: puppet diag: %s equipment +%#x: %.48s", who ? "puppet" : "player", o, (const char *)buf + o);
                        o += 6;
                    }
                }
            }
        }
    }
    if (dn && dn[0] == 'h' && player) {   // the player's own hair actor against the puppet's, 6 s after the puppet exists
        static int hair_done;
        if (!hair_done && now - known_ns > 6000000000ull) {
            hair_done = 1;
            static Part want;
            snprintf(want.name, sizeof want.name, "%s", getenv("TOTK_COOP_DIAG_PIECE") ? getenv("TOTK_COOP_DIAG_PIECE") : "Armor_Default_Extra_00");
            uint64_t found[2] = {0, 0};
            diag_found = 0, diag_bones("player (hair actors named)", player, &want), found[0] = diag_found;
            // the puppet's: what its equipment component's hair slot leads to
            uint64_t equip = read_ptr(read_ptr(actor + 0x228) + 70 * 8), slot[4] = {0};
            plat_mem_read(equip + 0x14a8, slot, sizeof slot);
            rt_log("coop: puppet diag: the puppet's hair slot holds %llx %llx %llx %llx", (unsigned long long)slot[0],
                   (unsigned long long)slot[1], (unsigned long long)slot[2], (unsigned long long)slot[3]);
            for (int k = 0; k < 4 && !found[1]; k++) {
                uint64_t cand[3] = {slot[k], read_ptr(slot[k] + 0x40), read_ptr(slot[k])};
                for (int j = 0; j < 3 && !found[1]; j++)
                    if (cand[j] > 0x10000 && part_named(&want, cand[j])) found[1] = cand[j];
            }
            if (!found[1]) diag_found = 0, diag_bones("puppet (hair actors named)", actor, &want), found[1] = diag_found;
            (void)found;
            {   // the update state of the four actors: the words at +0x140..+0x180 and the pause flags
                uint64_t who4[4] = {player, actor, diag_nhits > 0 ? diag_hits[0] : 0, diag_nhits > 1 ? diag_hits[1] : 0};
                static const char *const n4[4] = {"player", "puppet", "player's hair", "puppet's hair"};
                for (int i = 0; i < 4; i++) {
                    uint32_t w[16] = {0}, pf = 0, w1d0[4] = {0};
                    int32_t st = 0;
                    if (!who4[i]) continue;
                    plat_mem_read(who4[i] + 0x140, w, sizeof w), plat_mem_read(who4[i] + 0x5ac, &pf, 4);
                    plat_mem_read(who4[i] + 0x24, &st, 4), plat_mem_read(who4[i] + 0x1d0, w1d0, sizeof w1d0);
                    rt_log("coop: puppet diag: update state of the %s: state %d, pause %08x, +0x140: %x %x %x %x %x %x %x %x %x %x %x %x "
                           "%x %x %x %x, +0x1d0: %x %x %x %x", n4[i], st, pf, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9],
                           w[10], w[11], w[12], w[13], w[14], w[15], w1d0[0], w1d0[1], w1d0[2], w1d0[3]);
                }
            }            for (int who = 0; who < diag_nhits; who++) {
                uint64_t h = diag_hits[who], body = who ? actor : player;
                float hp[3] = {0}, bp[3] = {0}, mm[12] = {0};
                uint32_t f440[2] = {0}, f5ac = 0;
                int32_t st = 0;
                uint64_t model = read_ptr(read_ptr(read_ptr(h + 0x228) + 8) + 0x28), mw[16] = {0};
                plat_mem_read(h + 0x2b4, hp, sizeof hp), plat_mem_read(body + 0x2b4, bp, sizeof bp);
                plat_mem_read(h + 0x440, f440, sizeof f440), plat_mem_read(h + 0x5ac, &f5ac, 4), plat_mem_read(h + 0x24, &st, 4);
                plat_mem_read(model + 0x1f8, mm, sizeof mm), plat_mem_read(model, mw, sizeof mw);
                rt_log("coop: puppet diag: %s hair %#llx: pos %.2f %.2f %.2f (body %.2f %.2f %.2f), state %d, flags440 %08x %08x, "
                       "flags5ac %08x, model %#llx at %.2f %.2f %.2f", who ? "second" : "first", (unsigned long long)h, hp[0],
                       hp[1], hp[2], bp[0], bp[1], bp[2], st, f440[0], f440[1], f5ac, (unsigned long long)model, mm[3], mm[7], mm[11]);
                char line[600];
                int n = 0;
                for (int k = 0; k < 16; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %llx", (unsigned long long)mw[k]);
                rt_log("coop: puppet diag: %s hair model words:%s", who ? "second" : "first", line);
                uint32_t aw[24] = {0};
                plat_mem_read(h + 0x420, aw, sizeof aw);
                n = 0;
                for (int k = 0; k < 24; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %x", aw[k]);
                rt_log("coop: puppet diag: %s hair actor +0x420..:%s", who ? "second" : "first", line);
                uint32_t big[0x5c0 / 4] = {0};   // the whole actor, to compare the two word by word
                static uint32_t first[0x5c0 / 4];
                plat_mem_read(h, big, sizeof big);
                if (!who) memcpy(first, big, sizeof big);
                else {
                    n = 0;
                    for (unsigned k = 0; k < sizeof big / 4 && n < 520; k++)
                        if (big[k] != first[k] && (k & 1 ? 1 : (big[k] >> 28) == 0 && (first[k] >> 28) == 0))
                            n += snprintf(line + n, sizeof line - (size_t)n, " +%#x:%x/%x", k * 4, first[k], big[k]);
                    rt_log("coop: puppet diag: actor words that differ (first/second):%s", line);
                }
                // the same for the model, and for the object its fourth word points at (the model's units)
                static uint32_t mfirst[2][0x400 / 4];
                for (int part = 0; part < 2; part++) {
                    uint32_t mb[0x400 / 4] = {0};
                    uint64_t at = part ? mw[3] : model;
                    plat_mem_read(at, mb, sizeof mb);
                    if (!who) memcpy(mfirst[part], mb, sizeof mb);
                    else {
                        n = 0;
                        for (unsigned k = 0; k < sizeof mb / 4 && n < 540; k++)
                            if (mb[k] != mfirst[part][k] && (k & 1 ? (mb[k] | mfirst[part][k]) > 3 || 1 : (mb[k] >> 28) == 0 && (mfirst[part][k] >> 28) == 0))
                                if (!((k & 1) && (mb[k] == 1 || mb[k] == 3) && (mfirst[part][k] == 1 || mfirst[part][k] == 3)))   // pointer high halves
                                    n += snprintf(line + n, sizeof line - (size_t)n, " +%#x:%x/%x", k * 4, mfirst[part][k], mb[k]);
                        rt_log("coop: puppet diag: %s words that differ (first/second):%s", part ? "model unit" : "model", line);
                    }
                }
            }
        }
    }    if (dn && dn[0] == 'h' && diag_nhits >= 2 && diag_hits[1]) {
        // experiment: the flag words in which the puppet's hair differs from the player's, set to the player's values
        uint64_t h = diag_hits[1];
        int which = dn[1] ? dn[1] - '0' : 0;   // h1: +0x5ac, h2: also +0x440, h3: also +0x1d4 and +0x530
        if (which >= 1 && which <= 3) *(uint32_t *)(uintptr_t)(h + 0x5ac) &= ~1u;
        if (which >= 2 && which <= 3) *(uint32_t *)(uintptr_t)(h + 0x440) = 0x530300;
        if (which == 3) *(uint32_t *)(uintptr_t)(h + 0x1d4) |= 3, *(uint32_t *)(uintptr_t)(h + 0x530) = 1;
        uint64_t hm = read_ptr(read_ptr(read_ptr(h + 0x228) + 8) + 0x28);   // h4, h5: the model's words instead
        if (hm && which >= 4 && which <= 6) *(uint32_t *)(uintptr_t)(hm + 0x2ac) = 1, *(uint32_t *)(uintptr_t)(hm + 0x324) = 1;
        if (hm && which >= 5 && which <= 6) *(uint32_t *)(uintptr_t)(hm + 0x274) |= 0x400000, *(uint32_t *)(uintptr_t)(hm + 0x240) = 0x16810;
        if (hm && which == 7) *(uint32_t *)(uintptr_t)(hm + 0x240) &= ~0x6800u;   // h7: hide the piece as the game hides the player's quiver
        if (hm && which == 8) *(uint32_t *)(uintptr_t)(hm + 0x240) |= 0x8u;   // h8: the bit the draw pass skips on (main+0x8e53c)
    }    if (dn && dn[0] == 'h' && dn[1] == 'w' && diag_nhits >= 2) {   // who sets the show bits 0x6800 of the puppet's piece's model
        static int armed, reported;
        uint64_t hm = read_ptr(read_ptr(read_ptr(diag_hits[1] + 0x228) + 8) + 0x28);
        uint32_t mv = 0;
        if (!reported && now - known_ns > 11000000000ull) {
            reported = 1;
            watch_report();
        } else if (!reported && hm) {
            if (!armed) armed = 1, atomic_store(&watch_bit, 0x800u), atomic_store(&watch_keep, 1), watch_arm(hm + 0x240, 4);
            if (plat_mem_read(hm + 0x240, &mv, 4) && (mv & 0x6800)) *(volatile uint32_t *)(uintptr_t)(hm + 0x240) = mv & ~0x6800u;
        }
    }    if (dn && dn[0] == 'h' && dn[1] == 'q' && diag_nhits >= 2) {   // who sets the pause bit 1 of the puppet's hair: cleared here each present
        static int armed, reported;
        uint64_t h = diag_hits[1];
        uint32_t pf = 0;
        if (!reported && now - known_ns > 11000000000ull) {
            reported = 1;
            watch_report();
        } else if (!reported) {
            if (!armed) armed = 1, atomic_store(&watch_bit, 1u), atomic_store(&watch_keep, 1), watch_arm(h + 0x5ac, 4);
            if (plat_mem_read(h + 0x5ac, &pf, 4) && (pf & 1)) *(volatile uint32_t *)(uintptr_t)(h + 0x5ac) = pf & ~1u;
        }
    }    if (dn && dn[0] == 'p') {   // who sets the puppet's pause bit 0x8000 (actor +0x5ac): it is cleared here each present
        static int armed, reported;
        uint32_t pf = 0;
        if (!reported && now - known_ns > 8000000000ull) {
            reported = 1;
            watch_report();
        } else if (!reported && now - known_ns > 1500000000ull) {
            if (!armed) armed = 1, atomic_store(&watch_bit, 0x8000u), atomic_store(&watch_keep, 1), watch_arm(actor + 0x5ac, 4);
            if (plat_mem_read(actor + 0x5ac, &pf, 4) && (pf & 0x8000)) *(volatile uint32_t *)(uintptr_t)(actor + 0x5ac) = pf & ~0x8000u;
        }
    }
    if (dn && dn[0] == 'w') {
        static int reported;
        uint64_t loc = skel_locals(skel_block(actor, 0), skel_bones[0]);
        if (!reported && now - known_ns > 8000000000ull) {
            reported = 1;
            watch_report();
        } else if (!reported && loc && now - known_ns > 1500000000ull) {
            watch_arm(loc, skel_bones[0] * LOCAL_BYTES);
        }
    }
    if (!num_done && dn && dn[0] == 'n' && now - known_ns > 8500000000ull) {   // the numbers of both, late in the run
        num_done = 1;
        for (int who = 0; who < 2; who++) {
            uint64_t a = who ? actor : player;
            float rot[9] = {0}, ap[3] = {0}, mm[12] = {0};
            uint64_t model = read_ptr(read_ptr(read_ptr(a + 0x228) + 8) + 0x28);
            plat_mem_read(a + 0x2c0, rot, sizeof rot), plat_mem_read(a + 0x2b4, ap, sizeof ap);
            plat_mem_read(model + 0x1f8, mm, sizeof mm);
            rt_log("coop: puppet num: %s pos %.2f %.2f %.2f rot [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f] model %#llx "
                   "mtx [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]", who ? "puppet" : "player", ap[0],
                   ap[1], ap[2], rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8],
                   (unsigned long long)model, mm[0], mm[1], mm[2], mm[3], mm[4], mm[5], mm[6], mm[7], mm[8], mm[9], mm[10],
                   mm[11]);
            uint64_t loc = skel_locals(skel_block(a, !who), 63);
            for (int b = 0; b < 5; b++) {
                float f[12] = {0};
                uint32_t flag = 0;
                plat_mem_read(loc + (uint64_t)b * LOCAL_BYTES, f, sizeof f);
                memcpy(&flag, f, 4);
                rt_log("coop: puppet num: %s bone %d flag %08x scale %.2f %.2f %.2f trans %.3f %.3f %.3f (%.3f) rot %.3f %.3f "
                       "%.3f %.3f", who ? "puppet" : "player", b, flag, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9],
                       f[10], f[11]);
            }
        }
    }
    if (!diag_done && now - known_ns > 2000000000ull) {
        diag_done = 1;
        const char *d = cfg_get("TOTK_COOP_PUPPET_DIAG");
        if (d && d[0] == '1') {
            if (player) diag_actor("player", player), diag_skel("player", player);
            diag_actor("puppet", actor), diag_skel("puppet", actor);
            if (d[1] == 'b') {
                if (player) diag_bones("player", player, NULL);
                diag_bones("puppet", actor, NULL);
            }
        }
    }
}
