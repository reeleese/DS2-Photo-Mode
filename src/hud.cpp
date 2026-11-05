// The game's HUD while photo mode is on: hidden by default, Y brings it back. Leaving
// photo mode puts the game's own setting back.
#include "common.h"

namespace pm {

static bool g_want_shown;   // in photo mode: Y toggles this
static bool g_was_on;

static bool hud_read(bool* shown) { (void)shown; return false; }  // TODO: the game's flag
static void hud_write(bool shown) { (void)shown; }

static bool g_saved_valid, g_saved;

void hud_init() {}

void hud_toggle() { g_want_shown = !g_want_shown; }
bool hud_shown() { return g_want_shown; }

void hud_frame() {
    if (g_photo_mode && !g_was_on) {
        g_saved_valid = hud_read(&g_saved);
        g_want_shown = false;
    }
    if (!g_photo_mode && g_was_on && g_saved_valid) hud_write(g_saved);
    g_was_on = g_photo_mode;
    if (g_photo_mode) hud_write(g_want_shown);
}

}  // namespace pm
