// Keyboard and mouse for photo mode: the same actions as the pad, from keys and the mouse.
// Read once per frame on the render thread (kbm_poll adds them to the pad's state), only while
// the game's window is in the foreground. While photo mode is on, the game's own DirectInput
// keyboard and mouse are blanked (GetDeviceState / GetDeviceData hooks), so a key pressed for
// photo mode doesn't also act in the game; the mouse's movement and wheel are read there first.
#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include "common.h"
#include <dinput.h>
#include "MinHook.h"
#include <math.h>

namespace pm {

static volatile LONG g_dx, g_dy, g_wheel;  // mouse movement and wheel seen in the game's DirectInput reads
static volatile DWORD g_di_tick;           // when the last mouse read came through (0: none yet)
static bool g_kbm_last = false;            // the last input came from the keyboard or mouse

typedef HRESULT(STDMETHODCALLTYPE* GetStateFn)(IDirectInputDevice8W*, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE* GetDataFn)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetStateFn o_state_kb, o_state_ms;
static GetDataFn o_data_kb, o_data_ms;

// Wine (and Windows) give the keyboard and the mouse one IDirectInputDevice8 vtable, so the
// keyboard's hooks also see the mouse's reads: each read is sorted by the device's type.
enum DevKind { DEV_OTHER, DEV_KB, DEV_MOUSE };
static DevKind dev_kind(IDirectInputDevice8W* d) {
    static IDirectInputDevice8W* seen[8];
    static DevKind kinds[8];
    static int count;
    for (int i = 0; i < count; i++)
        if (seen[i] == d) return kinds[i];
    DIDEVICEINSTANCEW info = {};
    info.dwSize = sizeof(info);
    DevKind k = DEV_OTHER;
    if (SUCCEEDED(d->GetDeviceInfo(&info))) {
        BYTE t = GET_DIDEVICE_TYPE(info.dwDevType);
        k = t == DI8DEVTYPE_KEYBOARD ? DEV_KB : t == DI8DEVTYPE_MOUSE ? DEV_MOUSE : DEV_OTHER;
    }
    if (count < 8) {
        seen[count] = d;
        kinds[count++] = k;
    }
    return k;
}

static HRESULT state_read(GetStateFn orig, IDirectInputDevice8W* d, DWORD n, LPVOID p) {
    HRESULT r = orig(d, n, p);
    if (!g_photo_mode || FAILED(r) || !p) return r;
    DevKind k = dev_kind(d);
    if (k == DEV_MOUSE && n >= sizeof(DIMOUSESTATE)) {
        const DIMOUSESTATE* m = (const DIMOUSESTATE*)p;
        InterlockedAdd(&g_dx, m->lX);
        InterlockedAdd(&g_dy, m->lY);
        InterlockedAdd(&g_wheel, m->lZ);
        g_di_tick = GetTickCount() | 1;
    }
    if (k != DEV_OTHER) ZeroMemory(p, n);
    return r;
}
static HRESULT data_read(GetDataFn orig, IDirectInputDevice8W* d, DWORD sz, LPDIDEVICEOBJECTDATA p, LPDWORD n, DWORD f) {
    HRESULT r = orig(d, sz, p, n, f);
    if (!g_photo_mode || !p || !n || FAILED(r)) return r;
    DevKind k = dev_kind(d);
    if (k == DEV_MOUSE) {
        for (DWORD i = 0; i < *n; i++) {
            const DIDEVICEOBJECTDATA* e = (const DIDEVICEOBJECTDATA*)((const BYTE*)p + (size_t)i * sz);
            if (e->dwOfs == DIMOFS_X) InterlockedAdd(&g_dx, (LONG)e->dwData);
            else if (e->dwOfs == DIMOFS_Y) InterlockedAdd(&g_dy, (LONG)e->dwData);
            else if (e->dwOfs == DIMOFS_Z) InterlockedAdd(&g_wheel, (LONG)e->dwData);
        }
        if (*n) g_di_tick = GetTickCount() | 1;
    }
    if (k != DEV_OTHER) *n = 0;  // read (so they are consumed), report none
    return r;
}
static HRESULT STDMETHODCALLTYPE hk_state_kb(IDirectInputDevice8W* d, DWORD n, LPVOID p) { return state_read(o_state_kb, d, n, p); }
static HRESULT STDMETHODCALLTYPE hk_state_ms(IDirectInputDevice8W* d, DWORD n, LPVOID p) { return state_read(o_state_ms, d, n, p); }
static HRESULT STDMETHODCALLTYPE hk_data_kb(IDirectInputDevice8W* d, DWORD sz, LPDIDEVICEOBJECTDATA p, LPDWORD n, DWORD f) {
    return data_read(o_data_kb, d, sz, p, n, f);
}
static HRESULT STDMETHODCALLTYPE hk_data_ms(IDirectInputDevice8W* d, DWORD sz, LPDIDEVICEOBJECTDATA p, LPDWORD n, DWORD f) {
    return data_read(o_data_ms, d, sz, p, n, f);
}

static bool hook_slot(void* target, void* hook, void** orig) {
    MH_STATUS st = MH_CreateHook(target, hook, orig);
    if (st == MH_ERROR_ALREADY_CREATED) return true;  // the same function as the keyboard's: hooked already
    return st == MH_OK && MH_EnableHook(target) == MH_OK;
}

static void hook_device(IDirectInput8W* di, REFGUID guid, const char* what, void* hs, void** os, void* hd, void** od) {
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(di->CreateDevice(guid, &dev, nullptr)) || !dev) {
        logf("kbm: CreateDevice(%s) failed", what);
        return;
    }
    void** vt = *(void***)dev;
    bool ok = hook_slot(vt[9], hs, os);
    ok = hook_slot(vt[10], hd, od) && ok;
    logf("kbm: %s hooks %s", what, ok ? "in place" : "FAILED");
    dev->Release();
}

void kbm_init() {
    HMODULE m = GetModuleHandleW(L"dinput8.dll");
    if (!m) m = LoadLibraryW(L"dinput8.dll");
    typedef HRESULT(WINAPI * CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    CreateFn create = m ? (CreateFn)GetProcAddress(m, "DirectInput8Create") : nullptr;
    IDirectInput8W* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8W, (LPVOID*)&di, nullptr)) || !di) {
        logf("kbm: DirectInput not available: the game's keyboard is not blocked");
        return;
    }
    hook_device(di, GUID_SysKeyboard, "keyboard", (void*)hk_state_kb, (void**)&o_state_kb, (void*)hk_data_kb, (void**)&o_data_kb);
    hook_device(di, GUID_SysMouse, "mouse", (void*)hk_state_ms, (void**)&o_state_ms, (void*)hk_data_ms, (void**)&o_data_ms);
    di->Release();
}

static bool game_in_front() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool kbm_last_used() { return g_kbm_last; }
void kbm_pad_used() { g_kbm_last = false; }

// Adds the keyboard and mouse to `pad` (already holding the real pad, if any). `prev_buttons`
// is what it held last frame, to tell new presses.
void kbm_poll(Pad& pad, WORD prev_buttons, float dt) {
    static bool f9_prev;
    static bool had_cur;
    bool front = game_in_front();
    bool f9 = front && down(VK_F9);
    if (f9 && !f9_prev) {
        g_photo_mode = !g_photo_mode;
        g_kbm_last = true;  // opened from the keyboard: show the keyboard's hints
        logf("photo mode %s (F9)", g_photo_mode ? "on" : "off");
    }
    f9_prev = f9;
    if (!g_photo_mode || !front) {
        had_cur = false;
        InterlockedExchange(&g_dx, 0);
        InterlockedExchange(&g_dy, 0);
        InterlockedExchange(&g_wheel, 0);
        return;
    }

    WORD b = 0;
    float lx = 0, ly = 0;
    if (down('D')) lx += 1;
    if (down('A')) lx -= 1;
    if (down('W')) ly += 1;
    if (down('S')) ly -= 1;
    if (down('Q')) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (down('E')) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (down(VK_LEFT)) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (down(VK_RIGHT)) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (down(VK_UP)) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (down(VK_DOWN)) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (down('H')) b |= XINPUT_GAMEPAD_BACK;  // hide/show our panels
    if (down(VK_TAB)) b |= XINPUT_GAMEPAD_X;
    if (down(VK_BACK) || down('R')) b |= XINPUT_GAMEPAD_B;
    if (down(VK_RETURN) || down(VK_SPACE) || down(VK_LBUTTON)) b |= XINPUT_GAMEPAD_A;
    float rt = down(VK_SHIFT) ? 1.0f : 0, lt = down(VK_CONTROL) ? 1.0f : 0;

    // Mouse: the game's DirectInput movement when it is coming through, else the cursor's
    // movement from the window's centre (the cursor is put back there each frame).
    float mx = 0, my = 0;
    LONG wheel = InterlockedExchange(&g_wheel, 0);
    LONG ddx = InterlockedExchange(&g_dx, 0), ddy = InterlockedExchange(&g_dy, 0);
    bool di_live = g_di_tick && (GetTickCount() - g_di_tick) < 1000;
    if (di_live) {
        mx = (float)ddx;
        my = (float)ddy;
    } else {
        HWND w = GetForegroundWindow();
        RECT rc;
        POINT c;
        if (GetClientRect(w, &rc)) {
            POINT mid = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            ClientToScreen(w, &mid);
            if (GetCursorPos(&c)) {
                if (had_cur) {
                    mx = (float)(c.x - mid.x);
                    my = (float)(c.y - mid.y);
                }
                SetCursorPos(mid.x, mid.y);
                had_cur = true;
            }
        }
    }
    // A stick value is a turning rate; a mouse delta in pixels over dt is one. 1400 px/s is a full stick.
    const float PX_PER_SEC = 1400.0f;
    float rx = dt > 0 ? mx / (dt * PX_PER_SEC) : 0, ry = dt > 0 ? -my / (dt * PX_PER_SEC) : 0;
    if (rx > 4) rx = 4;
    if (rx < -4) rx = -4;
    if (ry > 4) ry = 4;
    if (ry < -4) ry = -4;

    bool used = b || lx || ly || rt || lt || rx || ry || wheel;
    if (!used) return;
    g_kbm_last = true;
    pad.buttons |= b;
    pad.pressed |= b & ~prev_buttons;
    if (lx) pad.lx = lx;
    if (ly) pad.ly = ly;
    if (rx) pad.rx = rx;
    if (ry) pad.ry = ry;
    if (rt) pad.rt = rt;
    if (lt) pad.lt = lt;
    pad.wheel += wheel / 120.0f;
}

}  // namespace pm
