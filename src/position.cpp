// The Position panel's engine: moves and turns where the character is DRAWN (posnmath.h),
// while the game's own character (physics, collision, the havok character body) stays where
// it stood. Writing her position for real makes the game drop her: lifted, she falls and
// plays the falling animation, and held up, the physics fights back. So
// only her models are moved, which also makes leaving photo mode exact: nothing of hers was
// changed, so there's nothing to put back.
//
// - Her models take their world matrix from the PlayerCtrl's getWorldMatrix(this, out)
//   (vtable +0x138: a copy of PlayerCtrl+0x60), at two places found with the dev channel's
//   page watch (`pgw`): the CharacterModelCtrl's (PlayerCtrl+0xF0) setter, and each
//   equipment part's model. The PlayerCtrl's vtable slot is replaced, and for those two
//   callers only (told by return address) the matrix handed back is offset. Its other
//   callers (twenty-odd: the game's own logic) get the true matrix: handing them the offset
//   one fed back into her position (she flew off).
// - Weapons (anything held at one of her bones) are placed by another function, from her
//   bone's matrix: it is hooked, and what it placed is carried along (posn_carry).
#include "common.h"
#include "posnmath.h"
#include "MinHook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pm {

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

// ---- the offset: set on the render thread, read on the game thread. Two slots: the
// render thread fills the one not in use, then switches.
static PosnOffset g_slot[2];
static volatile LONG g_cur;
static PosnOffset g_want;  // render thread: what the panel shows
static void publish() {
    PosnOffset o = g_photo_mode ? g_want : PosnOffset{};
    LONG next = 1 - g_cur;
    g_slot[next] = o;
    InterlockedExchange(&g_cur, next);
}
static PosnOffset current() { return g_slot[g_cur]; }

static const int WORLD_SLOT = 0x138 / 8;
static void* volatile g_player;            // the PlayerCtrl, refreshed on the render thread
static uintptr_t g_ret_model, g_ret_part;  // return addresses of the two model callers
static void** g_vt;                        // the patched vtable
static bool g_found;

typedef float*(__fastcall* WorldFn)(void* self, float* out);
static WorldFn o_world;
static volatile LONG g_hits, g_held_hits;  // (dev status)
static float* __fastcall hk_world(void* self, float* out) {
    float* r = o_world(self, out);
    if (self != g_player) return r;
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    if (ret != g_ret_model && ret != g_ret_part) return r;
    PosnOffset o = current();
    if (o.zero()) return r;
    InterlockedIncrement(&g_hits);
    Mat4 m;
    memcpy(&m, r, sizeof m);
    m = posn_apply(m, o);
    memcpy(r, &m, sizeof m);
    return r;
}

// The held part's update (rcx: the part; +0x08 its character, +0x18 its model, whose +0x10
// is the world matrix). It ends by setting the matrix from her bone's.
typedef void(__fastcall* HeldFn)(uint8_t* self);
static HeldFn o_held;
static void __fastcall hk_held(uint8_t* self) {
    o_held(self);
    void* pc = g_player;
    if (!self || !pc || *(void**)(self + 8) != pc || !o_world) return;
    PosnOffset o = current();
    if (o.zero()) return;
    uint8_t* model = *(uint8_t**)(self + 0x18);
    if (!model) return;
    alignas(16) Mat4 w;  // (the game stores it with aligned moves)
    o_world(pc, &w.m[0][0]);
    Mat4* t = (Mat4*)(model + 0x10);
    *t = posn_carry(*t, w, o);  // rigid, so the model's scale flag (set from it) holds
    InterlockedIncrement(&g_held_hits);
}

void position_init() {
    uint8_t* a = find_pattern("48 8B 03 48 8D 54 24 40 48 8B CB FF 90 38 01 00 00 48 8B 8B F0 00 00 00 48 8B D0 E8");
    uint8_t* b = find_pattern("48 8B 49 08 48 8D 54 24 20 48 8B 01 FF 90 38 01 00 00 48 8B 4E 18 0F 28 00 66 0F 7F 41 10");
    if (!a || !b) {
        logf("position: model matrix callers not found (unknown game version?)");
        return;
    }
    g_ret_model = (uintptr_t)a + 17;  // after their `call [rax+0x138]`
    g_ret_part = (uintptr_t)b + 18;
    logf("position: model matrix callers at rva %llx, %llx", (unsigned long long)(a - (uint8_t*)game_base()),
         (unsigned long long)(b - (uint8_t*)game_base()));
    uint8_t* h = find_pattern("48 8B C4 48 89 58 08 55 48 8D 68 A8 48 81 EC 50 01 00 00 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 "
                              "48 8B D9 48 8B 49 08 44 0F 29 50 A8 48 8B 01 FF 90 18 01 00 00");
    if (!h || MH_CreateHook(h, (void*)hk_held, (void**)&o_held) != MH_OK || MH_EnableHook(h) != MH_OK) {
        logf("position: held parts not found: the panel stays off (weapons would stay behind)");
        return;
    }
    g_found = true;
    logf("position: held parts at rva %llx", (unsigned long long)(h - (uint8_t*)game_base()));
}

// The PlayerCtrl's vtable, patched once a player exists (it's in the exe's read-only data).
static void patch(uint8_t* pc) {
    void** vt = (void**)deref(pc, 0);
    if (!vt || g_vt || !readable(vt + WORLD_SLOT, 8)) return;
    DWORD old;
    if (!VirtualProtect(vt + WORLD_SLOT, 8, PAGE_READWRITE, &old)) return;
    o_world = (WorldFn)vt[WORLD_SLOT];
    InterlockedExchangePointer(&vt[WORLD_SLOT], (void*)hk_world);
    VirtualProtect(vt + WORLD_SLOT, 8, old, &old);
    g_vt = vt;
    logf("position: getWorldMatrix at rva %llx, hooked", (unsigned long long)((uint8_t*)o_world - (uint8_t*)game_base()));
}

bool position_hooked() { return g_found; }

void position_frame(float) {
    static bool was_on;
    if (!g_found) return;
    uint8_t* pc = player_ctrl();
    if (pc) patch(pc);
    g_player = pc;
    if (was_on && !g_photo_mode) g_want = PosnOffset{};  // photo mode off: she's drawn where she is
    was_on = g_photo_mode;
    publish();
}

float position_get(int axis) {
    switch (axis) {
    case POSN_X: return g_want.x;
    case POSN_Y: return g_want.y;
    case POSN_Z: return g_want.z;
    case POSN_PITCH: return g_want.pitch;
    case POSN_YAW: return g_want.yaw;
    case POSN_ROLL: return g_want.roll;
    }
    return 0;
}
void position_set(int axis, float v) {
    switch (axis) {
    case POSN_X: g_want.x = v; break;
    case POSN_Y: g_want.y = v; break;
    case POSN_Z: g_want.z = v; break;
    case POSN_PITCH: g_want.pitch = v; break;
    case POSN_YAW: g_want.yaw = v; break;
    case POSN_ROLL: g_want.roll = v; break;
    }
}
void position_reset() { g_want = PosnOffset{}; }

#ifdef PM_DEV
// posn                       status
// posn X Y Z PITCH YAW ROLL  set the offset (metres, degrees), as the panel would
void position_dev(const char* a, char* out, size_t n) {
    float v[6] = {};
    if (sscanf(a, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 3; i++) position_set(POSN_X + i, v[i]);
        for (int i = 3; i < 6; i++) position_set(POSN_X + i, v[i] / 57.2957795f);
    }
    uint8_t* pc = (uint8_t*)g_player;
    uint8_t* c = deref(deref(pc, 0xF8), 0xF0);
    float* pq = c ? (float*)(c + 0x20) : nullptr;
    float* w = pc ? (float*)(pc + 0x60) : nullptr;
    uint8_t* model = deref(pc, 0xF0);
    float* mm = model ? (float*)(model + 0x20) : nullptr;
    PosnOffset o = g_want;
    snprintf(out, n,
             "hooked %d photo %d hits %ld held %ld | chr %.3f %.3f %.3f q %.3f %.3f %.3f %.3f | world %.3f %.3f %.3f | "
             "model %.3f %.3f %.3f up %.3f %.3f %.3f | off %.2f %.2f %.2f %.1f %.1f %.1f",
             (int)g_found, (int)g_photo_mode, (long)g_hits, (long)g_held_hits, pq ? pq[0] : 0.0f, pq ? pq[1] : 0.0f,
             pq ? pq[2] : 0.0f, pq ? pq[4] : 0.0f, pq ? pq[5] : 0.0f, pq ? pq[6] : 0.0f, pq ? pq[7] : 0.0f, w ? w[12] : 0.0f,
             w ? w[13] : 0.0f, w ? w[14] : 0.0f, mm ? mm[12] : 0.0f, mm ? mm[13] : 0.0f, mm ? mm[14] : 0.0f, mm ? mm[4] : 0.0f,
             mm ? mm[5] : 0.0f, mm ? mm[6] : 0.0f, o.x, o.y, o.z, o.pitch * 57.2957795f, o.yaw * 57.2957795f,
             o.roll * 57.2957795f);
}
#endif

}  // namespace pm
