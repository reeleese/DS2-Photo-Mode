// xinput1_3.dll proxy. DarkSoulsII.exe imports XInputGetState (ordinal 2) and
// XInputSetState (ordinal 3) from xinput1_3.dll, so a DLL of that name in the game
// folder loads with the game (under Proton, add the launch option
// WINEDLLOVERRIDES="xinput1_3=n,b" %command%). Every call goes on to the system XInput;
// while photo mode is on the game gets a neutral pad.
#include "common.h"

namespace pm {

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* SetStateFn)(DWORD, XINPUT_VIBRATION*);
typedef DWORD(WINAPI* GetCapsFn)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void(WINAPI* EnableFn)(BOOL);
typedef DWORD(WINAPI* GetGuidsFn)(DWORD, GUID*, GUID*);
typedef DWORD(WINAPI* GetBatteryFn)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD(WINAPI* GetKeystrokeFn)(DWORD, DWORD, PXINPUT_KEYSTROKE);

static HMODULE g_real;
static GetStateFn p_get_state, p_get_state_ex;
static SetStateFn p_set_state;
static GetCapsFn p_get_caps;
static EnableFn p_enable;
static GetGuidsFn p_get_guids;
static GetBatteryFn p_get_battery;
static GetKeystrokeFn p_get_keystroke;

static HMODULE load_system(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 32) return nullptr;
    path[n++] = L'\\';
    lstrcpyW(path + n, name);
    return LoadLibraryW(path);
}

bool xinput_init() {
    if (g_real) return true;
    // xinput1_4 first: same API, and no clash with this DLL's own module name.
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (auto* n : names) {
        HMODULE h = load_system(n);
        if (h && h != GetModuleHandleW(L"xinput1_3.dll")) {
            g_real = h;
            logf("xinput: using system %ls", n);
            break;
        }
    }
    if (!g_real) {
        logf("xinput: no system XInput found");
        return false;
    }
    p_get_state = (GetStateFn)GetProcAddress(g_real, "XInputGetState");
    p_get_state_ex = (GetStateFn)GetProcAddress(g_real, (LPCSTR)100);
    p_set_state = (SetStateFn)GetProcAddress(g_real, "XInputSetState");
    p_get_caps = (GetCapsFn)GetProcAddress(g_real, "XInputGetCapabilities");
    p_enable = (EnableFn)GetProcAddress(g_real, "XInputEnable");
    p_get_guids = (GetGuidsFn)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    p_get_battery = (GetBatteryFn)GetProcAddress(g_real, "XInputGetBatteryInformation");
    p_get_keystroke = (GetKeystrokeFn)GetProcAddress(g_real, "XInputGetKeystroke");
    return p_get_state != nullptr;
}

DWORD real_get_state(DWORD user, XINPUT_STATE* st) {
    if (!p_get_state && !xinput_init()) return ERROR_DEVICE_NOT_CONNECTED;
    return p_get_state(user, st);
}

// What the game sees: nothing while photo mode is on; outside it, the L3+R3 chord
// (photo mode's toggle) is held back once both sticks are down.
static void filter(XINPUT_STATE* st) {
    if (g_photo_mode) {
        ZeroMemory(&st->Gamepad, sizeof st->Gamepad);
        return;
    }
    const WORD chord = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    if ((st->Gamepad.wButtons & chord) == chord) st->Gamepad.wButtons &= ~chord;
}

}  // namespace pm

using namespace pm;

extern "C" {

DWORD WINAPI PM_XInputGetState(DWORD user, XINPUT_STATE* st) {
    DWORD r = real_get_state(user, st);
    if (r == ERROR_SUCCESS && st) filter(st);
    return r;
}

DWORD WINAPI PM_XInputGetStateEx(DWORD user, XINPUT_STATE* st) {
    if (!p_get_state && !xinput_init()) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = (p_get_state_ex ? p_get_state_ex : p_get_state)(user, st);
    if (r == ERROR_SUCCESS && st) filter(st);
    return r;
}

DWORD WINAPI PM_XInputSetState(DWORD user, XINPUT_VIBRATION* v) {
    if (!p_set_state && !xinput_init()) return ERROR_DEVICE_NOT_CONNECTED;
    return p_set_state ? p_set_state(user, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI PM_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    if (!p_get_caps && !xinput_init()) return ERROR_DEVICE_NOT_CONNECTED;
    return p_get_caps ? p_get_caps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI PM_XInputEnable(BOOL on) {
    if (!p_enable) xinput_init();
    if (p_enable) p_enable(on);
}

DWORD WINAPI PM_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    if (!p_get_guids) xinput_init();
    if (!p_get_guids) return ERROR_DEVICE_NOT_CONNECTED;
    return p_get_guids(user, render, capture);
}

DWORD WINAPI PM_XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* b) {
    if (!p_get_battery) xinput_init();
    if (!p_get_battery) return ERROR_DEVICE_NOT_CONNECTED;
    return p_get_battery(user, type, b);
}

DWORD WINAPI PM_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE k) {
    if (!p_get_keystroke) xinput_init();
    if (!p_get_keystroke) return ERROR_DEVICE_NOT_CONNECTED;
    if (g_photo_mode) return ERROR_EMPTY;
    return p_get_keystroke(user, reserved, k);
}

}  // extern "C"
