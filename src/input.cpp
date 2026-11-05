// The pad as photo mode reads it: straight from the system XInput, once per frame on
// the render thread, whatever the game itself is shown.
#include "common.h"
#include <math.h>

namespace pm {

volatile bool g_photo_mode = false;
static Pad g_pad;
static WORD g_real_buttons;  // the real pad only, for its own chord

static float stick(SHORT v, SHORT other, float dead) {
    // Radial-ish dead zone per axis, rescaled so the edge of the dead zone is 0.
    float f = v / 32767.0f;
    if (f < -1) f = -1;
    float a = fabsf(f);
    (void)other;
    if (a < dead) return 0;
    return (f > 0 ? 1 : -1) * (a - dead) / (1 - dead);
}

const Pad& pad_poll() {
    static float last_t;
    static LARGE_INTEGER freq, prev_t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = prev_t.QuadPart ? float(now.QuadPart - prev_t.QuadPart) / float(freq.QuadPart) : 1 / 60.0f;
    prev_t = now;
    if (dt <= 0 || dt > 0.25f) dt = 1 / 60.0f;
    (void)last_t;

    XINPUT_STATE st = {};
    DWORD user = 0;
    bool ok = false;
    for (; user < 4; user++)
        if (real_get_state(user, &st) == ERROR_SUCCESS) { ok = true; break; }
    WORD prev = g_pad.buttons;
    WORD prev_real = g_real_buttons;
    g_pad = Pad{};
    g_real_buttons = 0;
    if (ok) {
        const XINPUT_GAMEPAD& g = st.Gamepad;
        g_pad.connected = true;
        g_pad.buttons = g.wButtons;
        g_real_buttons = g.wButtons;
        g_pad.pressed = g.wButtons & ~prev;
        g_pad.lx = stick(g.sThumbLX, g.sThumbLY, 0.24f);
        g_pad.ly = stick(g.sThumbLY, g.sThumbLX, 0.24f);
        g_pad.rx = stick(g.sThumbRX, g.sThumbRY, 0.26f);
        g_pad.ry = stick(g.sThumbRY, g.sThumbRX, 0.26f);
        g_pad.lt = g.bLeftTrigger > 30 ? (g.bLeftTrigger - 30) / 225.0f : 0;
        g_pad.rt = g.bRightTrigger > 30 ? (g.bRightTrigger - 30) / 225.0f : 0;
        if (g.wButtons || g_pad.lx || g_pad.ly || g_pad.rx || g_pad.ry || g_pad.lt || g_pad.rt) kbm_pad_used();

        const WORD chord = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        if ((g.wButtons & chord) == chord && (prev_real & chord) != chord) {
            g_photo_mode = !g_photo_mode;
            logf("photo mode %s", g_photo_mode ? "on" : "off");
        }
    }
    kbm_poll(g_pad, prev, dt);  // keyboard and mouse: the same actions (F9 toggles photo mode)
    return g_pad;
}

}  // namespace pm
