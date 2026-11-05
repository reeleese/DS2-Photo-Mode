// Snap: a photo into the user's Steam screenshots for DS2, through the game's own
// steam_api64.dll. The overlay copies the game's frame (the 16:9 picture, before our
// panels are drawn on it) and hands the pixels to ISteamScreenshots::WriteScreenshot;
// if that can't be done, TriggerScreenshot asks Steam to take one itself, with the panels
// hidden meanwhile.
//
// The game's steam_api64.dll is old: no flat API, only the `SteamScreenshots` accessor
// returning ISteamScreenshots, whose vtable starts WriteScreenshot (0),
// AddScreenshotToLibrary (1), TriggerScreenshot (2).
#include "common.h"

namespace pm {

typedef void*(__cdecl* SteamScreenshotsFn)();
typedef uint32_t(__fastcall* WriteFn)(void* self, void* rgb, uint32_t size, int w, int h);
typedef void(__fastcall* TriggerFn)(void* self);

enum { IDLE, CAPTURE, TRIGGERED, SHOWN };
static int g_state = IDLE;
static float g_t;      // seconds in the current state
static float g_toast;  // seconds left showing the result
static const char* g_result = "";

static void* screenshots() {
    HMODULE h = GetModuleHandleW(L"steam_api64.dll");
    auto get = h ? (SteamScreenshotsFn)GetProcAddress(h, "SteamScreenshots") : nullptr;
    void* ss = get ? get() : nullptr;
    if (!ss) logf("snap: no ISteamScreenshots (steam_api64 %p)", (void*)h);
    return ss;
}

void snap_request() {
    if (g_state == IDLE) {
        g_state = CAPTURE;
        g_t = 0;
    }
}

int snap_frame(float dt) {
    g_t += dt;
    if (g_toast > 0) g_toast -= dt;
    switch (g_state) {
    case CAPTURE:
        return SNAP_CAPTURE | SNAP_HIDE;
    case TRIGGERED:  // Steam takes it within a frame or two
        if (g_t > 0.6f) { g_state = IDLE; g_toast = 2.5f; }
        return SNAP_HIDE;
    case SHOWN:  // a short blink of the panels, so the press is seen
        if (g_t > 0.15f) { g_state = IDLE; g_toast = 2.5f; }
        return SNAP_HIDE;
    }
    return 0;
}

void snap_captured(const uint8_t* rgb, int w, int h) {
    g_t = 0;
    void* ss = screenshots();
    if (ss && rgb) {
        uint32_t handle = ((WriteFn)(*(void***)ss)[0])(ss, (void*)rgb, (uint32_t)(w * h * 3), w, h);
        logf("snap: WriteScreenshot %dx%d -> handle %u", w, h, handle);
        if (handle) {
            g_result = "Saved to Steam screenshots";
            g_state = SHOWN;
            return;
        }
    }
    if (ss) {  // let Steam take it instead
        ((TriggerFn)(*(void***)ss)[2])(ss);
        logf("snap: TriggerScreenshot");
        g_result = "Saved to Steam screenshots";
        g_state = TRIGGERED;
        return;
    }
    g_result = "Steam screenshots unavailable";
    g_state = SHOWN;
}

const char* snap_toast() { return g_toast > 0 ? g_result : nullptr; }

}  // namespace pm
