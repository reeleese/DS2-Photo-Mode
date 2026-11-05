// Free camera.
//
// DS2's CameraManager (GameManagerImp+0x20) updates its active camera operator each
// frame, copies the operator's result into a camera slot (a 4x4 world matrix at +0x00,
// vertical field of view in radians at +0x50, near/far at +0x54/+0x58), and hands that
// slot to the renderer with one call (rva 0x39b070 in the Steam build, found by the
// pattern below: the call just before `cmp dword [rdi+0x48], 2` in CameraManager's
// update). That function is hooked: while photo mode is on, the slot gets our camera
// before the game passes it on. The operator itself is left alone, so leaving photo
// mode puts the game's own camera straight back.
#include "camera.h"
#include "MinHook.h"
#include <string.h>
#include <stdlib.h>

namespace pm {

// Slot layout (rows: right, up, forward, position; each padded to 4 floats).
struct alignas(16) Mat4 { float m[4][4]; };
static const size_t SLOT_FOV = 0x50;

static SRWLOCK g_lock = SRWLOCK_INIT;
static CamState g_cam;          // ours, while photo mode is on
static bool g_cam_valid;        // g_cam was seeded from the game camera
static Mat4 g_game_mat;         // the game's camera, last frame
static float g_game_fov = 0.7679f;
static bool g_hooked;

void camera_lock() { AcquireSRWLockExclusive(&g_lock); }
void camera_unlock() { ReleaseSRWLockExclusive(&g_lock); }
CamState& camera_state() { return g_cam; }
bool camera_hooked() { return g_hooked; }

Vec3 cam_forward(const CamState& c) { return look_dir(c.yaw, c.pitch); }

static void set_look(CamState& c, Vec3 dir) {
    Vec3 f = norm(dir);
    c.pitch = asinf(f.y < -1 ? -1 : f.y > 1 ? 1 : f.y);
    c.yaw = atan2f(f.x, f.z);
}

typedef void(__fastcall* PublishFn)(void* self, uint8_t* slot);
static PublishFn o_publish;

static Basis basis(const CamState& c) { return make_basis(c.yaw, c.pitch, c.roll); }
static void set_basis(CamState& c, const Basis& b) { basis_angles(b, c.yaw, c.pitch, c.roll); }

static void build(const CamState& c, Mat4& out) {
    Basis b = basis(c);
    Vec3 r = b.r, u = b.u, f = b.f;
    float rows[4][4] = {{r.x, r.y, r.z, 0}, {u.x, u.y, u.z, 0}, {f.x, f.y, f.z, 0}, {c.pos.x, c.pos.y, c.pos.z, 1}};
    memcpy(out.m, rows, sizeof rows);
}

// Where the character is: GameManagerImp (a global pointer, rva 0x16148F0 in the Steam
// build, found through the pattern in camera_init) +0xD0 is the PlayerCtrl, whose +0x60
// is the character's world matrix and +0x90 its position (feet). Confirmed in game.
static uintptr_t* g_gmi;  // &GameManagerImp
uintptr_t* game_manager_ptr() { return g_gmi; }

static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static bool player_position(Vec3* out) {
    if (!g_gmi || !readable(g_gmi, 8)) return false;
    uint8_t* gmi = (uint8_t*)*g_gmi;
    if (!gmi || !readable(gmi + 0xD0, 8)) return false;
    uint8_t* pc = *(uint8_t**)(gmi + 0xD0);
    if (!pc || !readable(pc + 0x90, 12)) return false;
    memcpy(out, pc + 0x90, 12);
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

void camera_reset() {
    const float(*m)[4] = g_game_mat.m;
    float speed = g_cam_valid ? g_cam.speed : 4.0f;
    bool orbit = g_cam.orbit;
    int flight = g_cam.flight;
    g_cam = CamState{};  // velocity and look rates back to zero too
    g_cam.speed = speed;
    g_cam.flight = flight;
    g_cam.pos = {m[3][0], m[3][1], m[3][2]};
    set_look(g_cam, Vec3{m[2][0], m[2][1], m[2][2]});
    g_cam.fov = g_game_fov;
    g_cam_valid = true;
    if (orbit) camera_set_orbit(true);
    logf("camera: reset to %.2f %.2f %.2f yaw %.1f pitch %.1f fov %.1f", g_cam.pos.x, g_cam.pos.y, g_cam.pos.z,
         g_cam.yaw * 57.2958f, g_cam.pitch * 57.2958f, g_cam.fov * 57.2958f);
}

void camera_set_orbit(bool on) {
    CamState& c = g_cam;
    c.orbit = on;
    if (!on) return;
    c.flight = FLIGHT_DEFAULT;
    Vec3 p;
    if (player_position(&p)) {
        c.pivot = p + Vec3{0, 1.2f, 0};  // about chest height
        c.orbit_dist = len(c.pos - c.pivot);
        if (c.orbit_dist < 0.5f) c.orbit_dist = 0.5f;
        set_look(c, c.pivot - c.pos);
    } else {
        c.orbit_dist = 3.5f;
        c.pivot = c.pos + cam_forward(c) * c.orbit_dist;
    }
}

void camera_set_flight(int f) {
    CamState& c = g_cam;
    c.flight = f;
    c.vel = {0, 0, 0};
    c.yaw_rate = c.pitch_rate = 0;
    if (f == FLIGHT_DEFAULT) return;
    c.orbit = false;
    if (f != FLIGHT_SEAMOTH) return;
    c.roll = 0;  // the Seamoth keeps the horizon level
    if (c.pitch > 1.55f) c.pitch = 1.55f;
    if (c.pitch < -1.55f) c.pitch = -1.55f;
}

static void __fastcall hk_publish(void* self, uint8_t* slot) {
    camera_lock();
    memcpy(&g_game_mat, slot, sizeof g_game_mat);
    g_game_fov = *(float*)(slot + SLOT_FOV);
    if (g_photo_mode && g_cam_valid) {
        Mat4 m;
        build(g_cam, m);
        memcpy(slot, &m, sizeof m);
        *(float*)(slot + SLOT_FOV) = g_cam.fov;
    }
    camera_unlock();
    o_publish(self, slot);
}

// ---- pattern scan over the game's code
uint8_t* find_pattern(const char* pat) {
    uint8_t bytes[128];
    bool wild[128];
    int n = 0;
    for (const char* p = pat; *p && n < 128;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') { wild[n] = true; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        char h[3] = {p[0], p[1], 0};
        wild[n] = false;
        bytes[n++] = (uint8_t)strtoul(h, nullptr, 16);
        p += 2;
    }
    uint8_t* base = (uint8_t*)game_base();
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    // The first .text only: the second one is the protection's.
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* s = base + sec->VirtualAddress;
        size_t n_sec = sec->Misc.VirtualSize;
        for (size_t k = 0; k + n <= n_sec; k++) {
            int j = 0;
            while (j < n && (wild[j] || s[k + j] == bytes[j])) j++;
            if (j == n) return s + k;
        }
        break;
    }
    return nullptr;
}

void camera_init() {
    // mov rax,[rip+GameManagerImp]; mov rax,[rax+0xD0] (the PlayerCtrl)
    if (uint8_t* g = find_pattern("48 8B 05 ?? ?? ?? ?? 48 8B 80 D0 00 00 00")) {
        g_gmi = (uintptr_t*)(g + 7 + *(int32_t*)(g + 3));
        logf("camera: GameManagerImp at rva %llx", (unsigned long long)((uint8_t*)g_gmi - (uint8_t*)game_base()));
    } else {
        logf("camera: GameManagerImp not found: orbit circles the point ahead instead");
    }
    // mov rcx,[rip+x]; call publish; cmp dword [rdi+0x48], 2
    uint8_t* site = find_pattern("48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 7F 48 02");
    if (!site) {
        logf("camera: CameraManager update not found (unknown game version?)");
        return;
    }
    uint8_t* call = site + 7;
    void* target = call + 5 + *(int32_t*)(call + 1);
    logf("camera: publish call at rva %llx -> rva %llx", (unsigned long long)(call - (uint8_t*)game_base()),
         (unsigned long long)((uint8_t*)target - (uint8_t*)game_base()));
    if (MH_CreateHook(target, (void*)hk_publish, (void**)&o_publish) != MH_OK || MH_EnableHook(target) != MH_OK) {
        logf("camera: hook failed");
        return;
    }
    g_hooked = true;
}

// The sticks, once per frame on the render thread.
void camera_frame(const Pad& pad, float dt) {
    static bool was_on;
    camera_lock();
    if (g_photo_mode && !was_on) camera_reset();
    was_on = g_photo_mode;
    if (g_photo_mode && g_cam_valid) {
        CamState& c = g_cam;
        const float look = 1.8f;           // rad/s at full stick
        float zoom = c.fov / 0.7679f;      // aim slower when zoomed in
        float mult = 1.0f + 3.0f * pad.rt - 0.75f * pad.lt;  // RT faster, LT slower
        float s = c.speed * mult * dt;
        float rise = ((pad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1.0f : 0.0f) -
                     ((pad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1.0f : 0.0f);
        if (c.orbit) {
            // Right stick and left stick X circle the pivot; left stick Y moves in and
            // out; RB/LB raise and lower the pivot (the camera with it).
            c.yaw += (pad.rx + pad.lx) * look * dt;
            c.pitch += pad.ry * look * dt;
            if (c.pitch > 1.5f) c.pitch = 1.5f;
            if (c.pitch < -1.5f) c.pitch = -1.5f;
            c.orbit_dist *= expf(-pad.ly * 0.9f * mult * dt);  // in and out by proportion: fine up close
            if (c.orbit_dist < 0.3f) c.orbit_dist = 0.3f;
            if (c.orbit_dist > 200.0f) c.orbit_dist = 200.0f;
            c.pivot = c.pivot + Vec3{0, rise * s, 0};
            c.pos = c.pivot - cam_forward(c) * c.orbit_dist;
        } else if (c.flight == FLIGHT_6DOF) {
            // Descent-style: everything relative to the view, no fixed horizon. Right stick
            // turns about the camera's own up and right axes, LB/RB roll about the view,
            // left stick flies along the view and strafes along the camera's right.
            Basis b = basis(c);
            float bank = ((pad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1.0f : 0.0f) -
                         ((pad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1.0f : 0.0f);  // RB: right side down
            turn(b.f, b.r, pad.rx * look * zoom * dt);
            turn(b.f, b.u, pad.ry * look * zoom * dt);
            turn(b.r, b.u, bank * 1.2f * dt);
            set_basis(c, b);
            b = basis(c);  // re-squared from the angles, so rounding never builds up
            c.pos = c.pos + b.f * (pad.ly * s) + b.r * (pad.lx * s);
        } else if (c.flight == FLIGHT_SEAMOTH) {
            // Subnautica's Seamoth: flies where it looks (pitch included) and strafes,
            // RB/LB rise and sink, the horizon stays level. It speeds up smoothly, glides
            // on after you let go and the water's drag slows it; the look lags slightly.
            c.yaw_rate = ease_to(c.yaw_rate, pad.rx * look * zoom, SEAMOTH_LOOK_TAU, dt);
            c.pitch_rate = ease_to(c.pitch_rate, pad.ry * look * zoom, SEAMOTH_LOOK_TAU, dt);
            c.yaw += c.yaw_rate * dt;
            c.pitch += c.pitch_rate * dt;
            if (c.pitch > 1.55f) { c.pitch = 1.55f; c.pitch_rate = 0; }
            if (c.pitch < -1.55f) { c.pitch = -1.55f; c.pitch_rate = 0; }
            float top = c.speed * mult;
            Vec3 want = (cam_forward(c) * pad.ly + yaw_right(c.yaw) * pad.lx + Vec3{0, rise, 0}) * top;
            float wl = len(want);
            if (wl > top) want = want * (top / wl);  // diagonals aren't faster
            seamoth_step(c.pos, c.vel, want, top, dt);
        } else {
            c.yaw += pad.rx * look * zoom * dt;
            c.pitch += pad.ry * look * zoom * dt;
            if (c.pitch > 1.55f) c.pitch = 1.55f;
            if (c.pitch < -1.55f) c.pitch = -1.55f;
            c.pos = c.pos + cam_forward(c) * (pad.ly * s) + yaw_right(c.yaw) * (pad.lx * s) + Vec3{0, rise * s, 0};
        }
        if (c.yaw > 3.14159265f) c.yaw -= 6.2831853f;
        if (c.yaw < -3.14159265f) c.yaw += 6.2831853f;
    }
    camera_unlock();
}

}  // namespace pm
