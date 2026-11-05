// The Pose panel's engine: plays the game's own gestures and event actions on the player,
// holds them, scrubs them and turns the character. The logic is in posemath.h; this file
// is the game side.
//
// - The player's Morpheme network is advanced once per update by
//   MorphemeMotionCtrl::startUpdate(this, float* dt, ...); with dt <= 0 the game skips the
//   update and the pose stays as it was. The hook changes dt for the player's network
//   only (PlayerCtrl+0xF8 -> +0x28), as pose_step says. To go back, it calls the
//   function's own MR::Network::startUpdate with a negative dt instead; the clip samplers
//   are hooked to keep that safe (below, and README "The pose engine").
// - Kneel in prayer, Pray and Sit at bonfire play their clips through a carrier gesture
//   (CLIPS, below).
// - While a pose is on, the player gets no root motion: MorphemeMotionCtrl's
//   trajectoryDeltaPos/Quat(this, out) are hooked to return none. (A held network would
//   otherwise repeat its last step every frame, and she slides away.)
// - PlayerCtrl+0xB8 is the character's action state; ChrEventActionMotionCtrl reads it
//   each frame: +0x730 the gesture category, sent with the Gesture request when flag 0x2000
//   of the u64 at +0x4C0 is set (the game clears it); +0x2A4 the event action category,
//   sent with the EventAction request while the counter +0x4E4 is non-zero; a step of the
//   counter +0x4E8 ends the event action.
// - PlayerCtrl+0xF8 -> +0xF0 is MorphemeChrCtrl: +0x30 and +0x50 hold the character's
//   orientation quaternion (x, y, z, w); writing both turns her.
#include "common.h"
#include "posemath.h"
#include "MinHook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace pm {

// Other clips played through a carrier gesture (CLIPS, below).
enum { CLIP_CARRIER, CLIP_PRAY_CROUCH, CLIP_PRAY_STAND, CLIP_BONFIRE_SIT, NCLIPS };

// The gestures by the game's own names (ItemParam's gesture items, in its menu's order) and
// categories (gesture N plays a74_01_<N>); Stretch (290) is the one gesture without an item.
// Then three clips played through the carrier gesture 130 (pose.cpp's CLIPS), each held at
// a frame when picked, and event action 510 (the a75_01 crouch a character wakes up from).
// dur: the clip's length in seconds (the Frame row's 100%). Each one checked in game.
static const PoseDef POSES[] = {
    {POSE_NONE, 0, 0, "None"},
    {POSE_GESTURE, 10, 4.33f, "Point"},
    {POSE_GESTURE, 20, 3.67f, "I won't bite"},
    {POSE_GESTURE, 30, 4.33f, "Bow"},
    {POSE_GESTURE, 40, 7.37f, "Welcome"},
    {POSE_GESTURE, 50, 6.10f, "Duel bow"},
    {POSE_GESTURE, 60, 5.00f, "Wave"},
    {POSE_GESTURE, 240, 6.33f, "Pumped up"},
    {POSE_GESTURE, 70, 5.17f, "Joy"},
    {POSE_GESTURE, 280, 8.00f, "Warcry"},
    {POSE_GESTURE, 270, 5.13f, "Warmup"},
    {POSE_GESTURE, 120, 4.50f, "Hurrah!"},
    {POSE_GESTURE, 80, 2.83f, "Righty-ho!"},
    {POSE_GESTURE, 90, 4.17f, "No way"},
    {POSE_GESTURE, 100, 3.33f, "This one's me"},
    {POSE_GESTURE, 130, 9.00f, "Have mercy!"},
    {POSE_GESTURE, 250, 8.00f, "Prostration"},
    {POSE_GESTURE, 150, 6.33f, "Decapitate"},
    {POSE_GESTURE, 310, 4.33f, "Fist pump"},
    {POSE_GESTURE, 220, 3.33f, "Mock"},
    {POSE_GESTURE, 300, 4.33f, "Praise the Sun"},
    {POSE_GESTURE, 290, 6.50f, "Stretch"},
#ifdef PM_HELD_POSES  // held back from 1.0 as too buggy; build with -DPM_HELD_POSES to list them
    {POSE_GESTURE, 130, 5.00f, "Kneel in prayer", 0.40f, CLIP_PRAY_CROUCH},
    {POSE_GESTURE, 130, 4.67f, "Pray", 0.50f, CLIP_PRAY_STAND},
    {POSE_GESTURE, 130, 2.17f, "Sit at bonfire", 0.97f, CLIP_BONFIRE_SIT},
#endif
    {POSE_EVENT, 510, 6.67f, "Crouch"},
};
static const int NPOSES = (int)(sizeof POSES / sizeof POSES[0]);

int pose_count() { return NPOSES; }
const char* pose_name(int i) { return i >= 0 && i < NPOSES ? POSES[i].name : "?"; }

// ---- reading the game
static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static uint8_t* deref(uint8_t* p, size_t off) {
    if (!p || !readable(p + off, 8)) return nullptr;
    return *(uint8_t**)(p + off);
}
static uint8_t* player_ctrl() {
    uintptr_t* g = game_manager_ptr();
    if (!g || !readable(g, 8)) return nullptr;
    return deref((uint8_t*)*g, 0xD0);
}
static uint8_t* player_motion() { return deref(deref(player_ctrl(), 0xF8), 0x28); }  // MorphemeMotionCtrl
static uint8_t* player_state() {
    uint8_t* s = deref(player_ctrl(), 0xB8);
    return s && readable(s, 0x740) ? s : nullptr;
}
static float* player_quats() {  // MorphemeChrCtrl +0x30 (the second copy at +0x50; positions at +0x20, +0x40)
    uint8_t* c = deref(deref(player_ctrl(), 0xF8), 0xF0);
    return c && readable(c + 0x20, 0x40) ? (float*)(c + 0x30) : nullptr;
}

// ---- state: the panel (render thread) asks, the hook (game thread) acts; both under g_lock
static CRITICAL_SECTION g_lock;
static bool g_lock_ok;
static PoseSM g_sm;
static void* volatile g_player_motion;  // refreshed each frame on the render thread
static void* g_sm_motion;               // the network g_sm was driving (a new one: start over)
static volatile LONG g_pin;              // no root motion for the player
static float g_turn;                    // wanted turn, radians from g_base
static float g_turn_applied;
static Quat g_base;
static bool g_base_ok;
static float g_hold[4];  // where she stood when the pose began (MorphemeChrCtrl +0x20)
static bool g_hold_ok;
static bool g_hooked;

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

static bool request_pending(uint8_t* st, int pose) {
    if (!st || pose <= 0 || pose >= NPOSES) return false;
    if (POSES[pose].kind == POSE_GESTURE) return (*(uint64_t*)(st + 0x4C0) & 0x2000) != 0;
    return *(int32_t*)(st + 0x4E4) != 0;
}

static void apply(uint8_t* st, const PoseOut& o) {
    if (!st || o.act == ACT_NONE || o.pose <= 0 || o.pose >= NPOSES) return;
    const PoseDef& d = POSES[o.pose];
    bool g = d.kind == POSE_GESTURE;
    switch (o.act) {
    case ACT_REQUEST:
        if (g) {
            *(int32_t*)(st + 0x730) = d.cat;
            *(uint64_t*)(st + 0x4C0) |= 0x2000;
        } else {
            *(int32_t*)(st + 0x2A4) = d.cat;
            *(int32_t*)(st + 0x4E4) += 1;
        }
        break;
    case ACT_END_EVENT:
        *(int32_t*)(st + 0x4E8) += 1;
        break;
    case ACT_CANCEL:
        if (g) *(uint64_t*)(st + 0x4C0) &= ~(uint64_t)0x2000;
        else if (*(int32_t*)(st + 0x4E4) > 0) *(int32_t*)(st + 0x4E4) -= 1;
        break;
    default:
        break;
    }
}

static void apply_turn() {  // under g_lock, game thread
    if (g_turn == g_turn_applied) return;
    float* q = player_quats();
    if (!q) return;
    if (!g_base_ok) {
        g_base = {q[0], q[1], q[2], q[3]};
        g_base_ok = true;
    }
    Quat t = turned(g_base, g_turn);
    q[0] = q[8] = t.x;
    q[1] = q[9] = t.y;
    q[2] = q[10] = t.z;
    q[3] = q[11] = t.w;
    g_turn_applied = g_turn;
    if (g_turn == 0) g_base_ok = false;  // back where she was: the next turn starts from there
}

// While a pose is on she stays where she stood: the root motion hooks stop most of it, but
// replays and fast-forwards still nudge her (a few cm each).
static void hold_position(bool on) {  // under g_lock, game thread
    if (!on) {
        g_hold_ok = false;
        return;
    }
    float* q = player_quats();
    if (!q) return;
    float* p = q - 4;  // +0x20, and the second copy at +0x40
    if (!g_hold_ok) {
        memcpy(g_hold, p, sizeof g_hold);
        g_hold_ok = true;
        return;
    }
    memcpy(p, g_hold, sizeof g_hold);
    memcpy(p + 8, g_hold, sizeof g_hold);
}

// ---- other clips in a gesture's place
// Poses without a request of their own (kneeling in prayer, sitting at a bonfire) play
// through a carrier gesture: while one is asked for, the network definition's source-anim
// attributes for the carrier's clip are pointed at the pose's clip instead, and put back
// once the pose is over. A source-anim attribute (Morpheme AttribDataSourceAnim, type 0x15)
// is 16 bytes of header (`ff ff 15 00` at +8), then the clip (+0x10), its skeleton map, ... and
// its duration (+0x88): the 0x90 bytes from +0x10 are what's copied. They sit in the
// network definition's block, a few MB after it (PlayerCtrl+0xF8 -> +0x28 -> +0x20 -> +0).
// A clip is told by its own numbers (c0001.anibnd): its duration
// (+0x28) and the first two pos-means quantisation floats (+0x60, +0x64).
struct ClipId {
    const char* name;
    float dur, q0, q1;
};
static const ClipId CLIPS[] = {
    {"a74_01_0130 Pray (the carrier: gesture 130, Have mercy!)", 9.0f, -0.104337998f, -0.260015488f},
    {"a70_11_0010 PrayCrouch", 5.0f, -0.104010001f, -0.00119516999f},
    {"a70_18_0010 PrayStand", 4.6667f, -0.036448054f, -0.00111224002f},
    {"a76_01_0010 BonfireStart", 2.1667f, -0.0658640265f, -0.0126270717f},
};
static const size_t ATTR_BYTES = 0x90;

static uint8_t* g_clip_def;              // the network definition the addresses below are in
static uint8_t* g_clip_src[NCLIPS];      // a source-anim attribute of each clip
static uint8_t* g_carrier[8];            // the carrier's source-anim attributes
static uint8_t g_carrier_orig[8][ATTR_BYTES];
static int g_ncarrier;
static int g_clip_in;                    // which clip the carrier plays now (0: its own)
static float g_clip_left = -1;           // idle: seconds until the carrier gets its clip back

static bool clip_is(uint8_t* anim, const ClipId& c) {
    if (!anim || !readable(anim, 0x70)) return false;
    float d = *(float*)(anim + 0x28), a = *(float*)(anim + 0x60), b = *(float*)(anim + 0x64);
    return fabsf(d - c.dur) < 0.001f && fabsf(a - c.q0) <= 1e-6f + 1e-5f * fabsf(c.q0) &&
           fabsf(b - c.q1) <= 1e-6f + 1e-5f * fabsf(c.q1);
}

static bool writable(void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!readable(p, n) || !VirtualQuery(p, &mi, sizeof mi)) return false;
    return (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
}

// Finds the clips' attributes in `def`'s block (up to 16 MB of readable memory after it).
// It also lists every clip the network has (g_anims, sorted), for the samplers below.
static const int MAX_ANIMS = 16384;
static uintptr_t g_anims[MAX_ANIMS];
static volatile LONG g_nanims;
static int cmp_uptr(const void* a, const void* b) {
    uintptr_t x = *(const uintptr_t*)a, y = *(const uintptr_t*)b;
    return x < y ? -1 : x > y;
}
static void clip_scan(uint8_t* def) {
    InterlockedExchange(&g_nanims, 0);
    int nanims = 0;
    memset(g_clip_src, 0, sizeof g_clip_src);
    g_ncarrier = 0;
    g_clip_in = 0;
    g_clip_def = def;
    if (!def) return;
    uint8_t* p = (uint8_t*)((uintptr_t)def & ~(uintptr_t)15);
    uint8_t* end = p + (16u << 20);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            break;
        uint8_t* re = (uint8_t*)mi.BaseAddress + mi.RegionSize;
        if (re > end) re = end;
        for (; p + 0x10 + ATTR_BYTES <= re; p += 8) {
            if (*(uint32_t*)(p + 8) != 0x0015FFFFu) continue;
            uint8_t* anim = *(uint8_t**)(p + 0x10);
            if ((uintptr_t)anim < 0x10000 || ((uintptr_t)anim & 7)) continue;
            if (nanims < MAX_ANIMS) g_anims[nanims++] = (uintptr_t)anim;
            for (int k = 0; k < NCLIPS; k++) {
                if (!clip_is(anim, CLIPS[k])) continue;
                if (k == CLIP_CARRIER) {
                    if (g_ncarrier < 8 && writable(p + 0x10, ATTR_BYTES)) {
                        g_carrier[g_ncarrier] = p;
                        memcpy(g_carrier_orig[g_ncarrier], p + 0x10, ATTR_BYTES);
                        g_ncarrier++;
                    }
                } else if (!g_clip_src[k]) {
                    g_clip_src[k] = p;
                }
            }
        }
        p = re;
    }
    qsort(g_anims, nanims, sizeof g_anims[0], cmp_uptr);
    InterlockedExchange(&g_nanims, nanims);
    logf("pose: clips: %d in the network", nanims);
    logf("pose: clips: %d carrier attribute(s); sources %s %s %s", g_ncarrier, g_clip_src[1] ? "yes" : "no",
         g_clip_src[2] ? "yes" : "no", g_clip_src[3] ? "yes" : "no");
}

// The carrier plays clip k (0: its own) from its next request on.
static void clip_for(uint8_t* net, int k) {  // under g_lock, game thread
    uint8_t* def = net ? deref(net, 0) : nullptr;
    if (def != g_clip_def) clip_scan(def);
    if (k == g_clip_in || (k && !g_clip_src[k])) return;
    for (int i = 0; i < g_ncarrier; i++)
        memcpy(g_carrier[i] + 0x10, k ? g_clip_src[k] + 0x10 : g_carrier_orig[i], ATTR_BYTES);
    g_clip_in = k;
    g_clip_left = -1;
}

// Once no pose of ours plays (handed back: it plays out first), the carrier gets its own
// clip back, so the gesture itself is as the game has it.
static void clip_settle(uint8_t* net, bool idle, float dt) {  // under g_lock, game thread
    if (!g_clip_in || !idle) {
        g_clip_left = -1;
        return;
    }
    if (g_clip_left < 0) g_clip_left = CLIPS[g_clip_in].dur + 1.0f;
    if ((g_clip_left -= (dt > 0 ? dt : 1 / 60.0f)) <= 0) clip_for(net, 0);
}

// ---- hooks
typedef void(__fastcall* StartUpdateFn)(void* self, float* dt, void* a3, uint8_t a4, uint8_t a5, uint8_t a6);
static StartUpdateFn o_start_update;
// MR::Network::startUpdate(net, dt, absTime, ...), which the hooked function calls only for
// dt > 0. Called with a negative dt, the network plays backwards.
typedef void(__fastcall* NetUpdateFn)(void* net, float dt, bool abs, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7);
static NetUpdateFn g_net_update;
static int g_rev_mode;  // dev: 1 replays instead of playing backwards

// What the hooked function does with dt > 0, with a step back instead.
static void net_update_back(void* self, float back, void* a3, uint8_t a4, uint8_t a5, uint8_t a6) {
    uint8_t* net = *(uint8_t**)((uint8_t*)self + 0x20);
    if (!net) return;
    *(void**)(net + 0xE0) = a3;
    g_net_update(net, -back, false, a4, a5, a6, 0);
}

static float take_young();
static thread_local bool g_in_update;  // the player's network is executing its update (its clips are sampled)
static void __fastcall hk_start_update(void* self, float* dt, void* a3, uint8_t a4, uint8_t a5, uint8_t a6) {
    if (self && self == g_player_motion && g_lock_ok) {
        float d, back;
        {
            Lock l;
            uint8_t* net = deref((uint8_t*)self, 0x20);
            if (self != g_sm_motion) {  // a new player (loaded in): nothing of ours is playing
                g_sm = PoseSM{};
                g_sm_motion = self;
                g_base_ok = false;
                g_turn = g_turn_applied = 0;
            }
            uint8_t* st = player_state();
            uint8_t* def = net ? deref(net, 0) : nullptr;
            if (def != g_clip_def) clip_scan(def);  // (a new definition: the old one's addresses are left alone)
            float young = take_young();  // (none sampled: the network was held, as it was)
            if (!g_nanims) g_sm.young = 0;
            else if (young < 1e30f) g_sm.young = young;
            g_sm.reverse = g_net_update && g_rev_mode != 1;
            PoseOut o = pose_step(g_sm, POSES, *dt, request_pending(st, g_sm.cur));
            if (o.act == ACT_REQUEST) clip_for(net, POSES[o.pose].kind == POSE_GESTURE ? POSES[o.pose].clip : 0);
            apply(st, o);
            clip_settle(net, g_sm.phase == PH_IDLE, *dt);
            g_pin = g_sm.phase != PH_IDLE;
            apply_turn();
            hold_position(g_sm.phase != PH_IDLE);
            d = o.dt;
            back = o.back;
        }
        if (back > 0) net_update_back(self, back, a3, a4, a5, a6);
        else o_start_update(self, &d, a3, a4, a5, a6);
        return;
    }
    o_start_update(self, dt, a3, a4, a5, a6);
}

typedef float*(__fastcall* DeltaFn)(void* self, float* out);
static DeltaFn o_delta_pos, o_delta_quat;
static float* __fastcall hk_delta_pos(void* self, float* out) {
    float* r = o_delta_pos(self, out);
    if (g_pin && self == g_player_motion) out[0] = out[1] = out[2] = out[3] = 0;
    return r;
}
static float* __fastcall hk_delta_quat(void* self, float* out) {
    float* r = o_delta_quat(self, out);
    if (g_pin && self == g_player_motion) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
    }
    return r;
}

// The clip samplers (MR::AnimSourceNSA: the whole pose, and one transform), clamped to the
// clip. Played backwards, a node that has only just started (the blend into the idle after a
// gesture) can be set before its own start, and NSA turns a negative time into a frame index
// far outside the clip: the game crashed reading it. The game itself never asks outside.
typedef void(__fastcall* SampleFn)(void* anim, float t, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                                   void* a9, void* a10);
static SampleFn o_sample_all, o_sample_one;
static volatile long g_clamped;  // samples clamped (dev status)
// Playing backwards must not take a node of the network back past its own start: the clamp
// above keeps the samplers safe, but the network's own update crashed too (a transition into
// the idle that had just begun). So the samplers note how far into its clip the youngest clip
// of the player's network was (the least time sampled from one of g_anims, while the player's
// network executes its update: other characters share the clips), and the pose goes back no
// further than that (pose_step's `young`). One-frame clips (additive poses, sampled at their
// only frame) say nothing about when their node began and are left out.
static volatile LONG g_young_bits = 0x7F7FFFFF;  // float bits (>= 0, so they order as ints)
// MorphemeMotionCtrl's update after startUpdate: runs the network's task queue
// (MR::Network::update until done), on the calling thread; the clips are sampled in it.
typedef void(__fastcall* ExecFn)(void* self, void* a2, void* a3, void* a4);
static ExecFn o_exec;
static void __fastcall hk_exec(void* self, void* a2, void* a3, void* a4) {
    bool me = self && self == g_player_motion;
    if (me) g_in_update = true;
    o_exec(self, a2, a3, a4);
    if (me) g_in_update = false;
}
static bool player_clip(void* anim) {
    LONG n = g_nanims;
    uintptr_t a = (uintptr_t)anim;
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_anims[mid] < a) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && g_anims[lo] == a;
}
static void note_young(void* anim, float t) {
    if (!g_in_update || *(float*)((uint8_t*)anim + 0x28) < 0.1f || !player_clip(anim)) return;
    if (!(t > 0)) t = 0;
    LONG v, bits;
    memcpy(&bits, &t, 4);
    while ((v = g_young_bits) > bits && InterlockedCompareExchange(&g_young_bits, bits, v) != v) {
    }
}
// The youngest clip's time since the last call (huge: none sampled).
static float take_young() {
    LONG bits = InterlockedExchange(&g_young_bits, 0x7F7FFFFF);
    float t;
    memcpy(&t, &bits, 4);
    return t;
}
#ifdef PM_DEV
struct DevSample { void* anim; float t; DWORD tid; void* a4; int n; bool in; void* a7; void* a8; };
static DevSample g_dev_samples[48];
static volatile LONG g_dev_lock;
static void dev_sample(void* anim, float t, void* a4, void* a7 = 0, void* a8 = 0) {
    if (!player_clip(anim)) return;
    while (InterlockedExchange(&g_dev_lock, 1)) {
    }
    int i = 0;
    bool in = g_in_update;
    for (; i < 48 && g_dev_samples[i].anim; i++)
        if (g_dev_samples[i].anim == anim && g_dev_samples[i].in == in) break;
    if (i < 48) g_dev_samples[i] = {anim, t, GetCurrentThreadId(), a4, g_dev_samples[i].n + 1, in, a7, a8};
    InterlockedExchange(&g_dev_lock, 0);
}
#endif
static inline float clip_time(void* anim, float t) {
    note_young(anim, t);
    float dur = *(float*)((uint8_t*)anim + 0x28);
    if (t >= 0 && t <= dur) return t;
    InterlockedIncrement(&g_clamped);
    return t > dur ? dur : 0;  // (NaN too)
}
static void __fastcall hk_sample_all(void* anim, float t, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                                     void* a9, void* a10) {
#ifdef PM_DEV
    dev_sample(anim, t, a4, a7, a8);
#endif
    o_sample_all(anim, clip_time(anim, t), a3, a4, a5, a6, a7, a8, a9, a10);
}
static void __fastcall hk_sample_one(void* anim, float t, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                                     void* a9, void* a10) {
#ifdef PM_DEV
    dev_sample(anim, t, (void*)1);
#endif
    o_sample_one(anim, clip_time(anim, t), a3, a4, a5, a6, a7, a8, a9, a10);
}

static uint8_t* g_hooked_at;
static bool hook(const char* what, const char* pat, void* fn, void** orig) {
    uint8_t* f = find_pattern(pat);
    g_hooked_at = f;
    if (!f) {
        logf("pose: %s not found (unknown game version?)", what);
        return false;
    }
    if (MH_CreateHook(f, fn, orig) != MH_OK || MH_EnableHook(f) != MH_OK) {
        logf("pose: %s hook failed", what);
        return false;
    }
    logf("pose: %s at rva %llx", what, (unsigned long long)(f - (uint8_t*)game_base()));
    return true;
}

void pose_init() {
    InitializeCriticalSection(&g_lock);
    g_lock_ok = true;
    // The root motion hooks go in first: the update hook only changes anything once they're in.
    bool pos = hook("trajectory delta pos",
                    "40 53 48 81 EC 80 00 00 00 0F 28 05 ?? ?? ?? ?? 48 8B DA 66 0F 7F 02 48 8B 49 20 48 85 C9 0F 84",
                    (void*)hk_delta_pos, (void**)&o_delta_pos);
    bool quat = hook("trajectory delta quat",
                     "40 53 48 83 EC 30 0F 28 05 ?? ?? ?? ?? 48 8B DA 66 0F 7F 02 48 8B 49 20 48 85 C9 0F 84",
                     (void*)hk_delta_quat, (void**)&o_delta_quat);
    if (!pos || !quat) return;
    g_hooked = hook("Morpheme update",
                    "48 83 EC 48 48 8B 41 20 48 85 C0 74 36 F3 0F 10 0A 0F 57 C0 0F 2F C8 76 2A 4C 89 80 E0 00 00 00",
                    (void*)hk_start_update, (void**)&o_start_update);
    // Its call to MR::Network::startUpdate (e8 rel32 at +0x3E), for playing backwards.
    // MinHook moved the first bytes; the call is past them, so it reads as it was.
    uint8_t* f = g_hooked_at;
    bool clamp = g_hooked &&
                 hook("network execute",
                      "40 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B 41 20 48 8B F9 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 5C",
                      (void*)hk_exec, (void**)&o_exec) &&
                 hook("clip sampler",
                      "48 8B C4 4C 89 48 20 41 54 41 57 48 83 EC 78 4C 8B 41 40 4C 8B BC 24 C0 00 00 00 48 89 58 10 48 8B 99 90 00 00 00",
                      (void*)hk_sample_all, (void**)&o_sample_all) &&
                 hook("clip transform sampler",
                      "48 8B C4 4C 89 48 20 4C 89 40 18 57 41 54 48 81 EC D8 00 00 00 48 89 58 08 48 89 68 E8 48 89 70 E0 4C 89 68 D8 4D 8B 69 20",
                      (void*)hk_sample_one, (void**)&o_sample_one);
    if (g_hooked && f && f[0x3E] == 0xE8 && clamp) {
        g_net_update = (NetUpdateFn)(f + 0x43 + *(int32_t*)(f + 0x3F));
        logf("pose: network update at rva %llx", (unsigned long long)((uint8_t*)g_net_update - (uint8_t*)game_base()));
    } else if (g_hooked) {
        logf("pose: network update call not found: going back replays");
    }
}

bool pose_hooked() { return g_hooked; }

// ---- the panel (render thread)
// Stepping through the list asks for a pose only once it rests on one for a moment: each
// change of pose has to play the last one out first.
static int g_pick = -1;      // shown in the panel, not asked for yet
static float g_pick_wait;    // seconds until it is
static const float PICK_REST = 0.35f;

static void flush_pick() {  // under g_lock
    if (g_pick < 0) return;
    pose_pick(g_sm, POSES, g_pick);
    g_pick = -1;
}

int pose_selected() {
    if (!g_lock_ok) return 0;
    Lock l;
    return g_pick >= 0 ? g_pick : g_sm.want;
}
void pose_select(int i) {
    if (!g_lock_ok || i < 0 || i >= NPOSES) return;
    Lock l;
    g_pick = i;
    g_pick_wait = PICK_REST;
}
float pose_frame_fraction() {
    if (!g_lock_ok) return 0;
    Lock l;
    if (g_pick >= 0) return 0;
    return pose_shown_fraction(g_sm, POSES);
}
void pose_set_frame_fraction(float f) {
    if (!g_lock_ok) return;
    Lock l;
    flush_pick();
    const PoseDef& d = POSES[g_sm.want];
    if (!g_sm.want || d.dur <= 0) return;
    g_sm.target = pose_clampf(f, 0, 1) * d.dur;
}
bool pose_frozen() {
    if (!g_lock_ok) return false;
    Lock l;
    return g_sm.frozen || g_sm.target >= 0;
}
void pose_set_freeze(bool on) {
    if (!g_lock_ok) return;
    Lock l;
    flush_pick();
    g_sm.frozen = on;
    if (!on) g_sm.target = -1;
}
float pose_turn() {
    if (!g_lock_ok) return 0;
    Lock l;
    return g_turn;
}
void pose_set_turn(float rad) {
    if (!g_lock_ok) return;
    Lock l;
    g_turn = rad;
}

// Photo mode off: the character goes back to the game as it was (pose ended, network at
// the game's speed, facing where she faced).
static void release() {
    Lock l;
    g_pick = -1;
    g_sm.want = 0;
    g_sm.target = -1;
    g_sm.frozen = false;
    g_turn = 0;
}

void pose_frame(float dt) {
    static bool was_on;
    if (!g_hooked) return;
    g_player_motion = player_motion();
    if (was_on && !g_photo_mode) release();
    was_on = g_photo_mode;
    Lock l;
    if (g_pick >= 0 && (g_pick_wait -= dt) <= 0) flush_pick();
}

#ifdef PM_DEV
void pose_dev(const char* a, char* out, size_t n) {
    char cmd[32] = {};
    while (*a == ' ') a++;
    sscanf(a, "%31s", cmd);
    const char* r = a + strlen(cmd);
    uint8_t* st = player_state();
    if (!strcmp(cmd, "select")) {
        pose_select(atoi(r));
        Lock l;
        flush_pick();
    } else if (!strcmp(cmd, "frame")) {
        pose_set_frame_fraction((float)atof(r));
    } else if (!strcmp(cmd, "freeze")) {
        pose_set_freeze(atoi(r) != 0);
    } else if (!strcmp(cmd, "turn")) {
        pose_set_turn((float)atof(r) / 57.2957795f);
    } else if (!strcmp(cmd, "clips")) {  // the player's clips sampled since the last "clips"
        while (InterlockedExchange(&g_dev_lock, 1)) {
        }
        size_t k = 0;
        for (int i = 0; i < 48 && g_dev_samples[i].anim && k + 80 < n; i++) {
            DevSample& d = g_dev_samples[i];
            k += snprintf(out + k, n - k, "%p t %.3f dur %.3f n %d tid %lu in %d a7 %p a8 %p\n", d.anim, (double)d.t,
                          (double)*(float*)((uint8_t*)d.anim + 0x28), d.n, (unsigned long)d.tid, (int)d.in, d.a7, d.a8);
        }
        memset(g_dev_samples, 0, sizeof g_dev_samples);
        InterlockedExchange(&g_dev_lock, 0);
        return;
    } else if (!strcmp(cmd, "revmode")) {
        Lock l;
        g_rev_mode = atoi(r);
    } else if (!strcmp(cmd, "release")) {
        release();
    } else if (!strcmp(cmd, "gesture") && st) {  // raw requests, outside the panel's logic
        *(int32_t*)(st + 0x730) = atoi(r);
        *(uint64_t*)(st + 0x4C0) |= 0x2000;
    } else if (!strcmp(cmd, "event") && st) {
        *(int32_t*)(st + 0x2A4) = atoi(r);
        *(int32_t*)(st + 0x4E4) += 1;
    } else if (!strcmp(cmd, "eventend") && st) {
        *(int32_t*)(st + 0x4E8) += 1;
    }
    float* q = player_quats();
    uint8_t* cc = deref(deref(player_ctrl(), 0xF8), 0xF0);
    float* pos = cc && readable(cc + 0x20, 16) ? (float*)(cc + 0x20) : nullptr;
    Lock l;
    snprintf(out, n,
             "pos %.3f %.3f %.3f | quat %.3f %.3f %.3f %.3f | want %d cur %d phase %d clock %.3f target %.2f frozen %d pin %ld turn %.1f | "
             "flags %llx evcount %d | replay %d clip %d carriers %d clamped %ld young %.3f exit %.2f",
             pos ? pos[0] : 0.0, pos ? pos[1] : 0.0, pos ? pos[2] : 0.0, q ? q[0] : 0.0, q ? q[1] : 0.0, q ? q[2] : 0.0,
             q ? q[3] : 0.0, g_sm.want, g_sm.cur, (int)g_sm.phase, (double)g_sm.clock, (double)g_sm.target, (int)g_sm.frozen,
             (long)g_pin, (double)(g_turn * 57.2957795f), st ? (unsigned long long)*(uint64_t*)(st + 0x4C0) : 0ull,
             st ? *(int32_t*)(st + 0x4E4) : -1, g_rev_mode, g_clip_in, g_ncarrier, (long)g_clamped, (double)g_sm.young, (double)g_sm.exit[g_sm.cur & (POSE_MAX - 1)]);
}
#endif

}  // namespace pm
