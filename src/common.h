// Shared declarations for the DS2 photo-mode DLL.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdint.h>

namespace pm {

// log.cpp: photo-mode.log in data_dir().
void log_init(HMODULE self);
void logf(const char* fmt, ...);
const wchar_t* data_dir();  // %LOCALAPPDATA%\ds2-photo-mode\ (trailing backslash), or "" if none

// xinput_proxy.cpp
bool xinput_init();                       // loads the system XInput the game would have used
DWORD real_get_state(DWORD user, XINPUT_STATE* st);  // the pad as it really is

// input.cpp: the pad as photo mode sees it, read once per frame on the render thread.
struct Pad {
    bool connected = false;
    WORD buttons = 0, pressed = 0;  // held, and newly pressed this frame
    float wheel = 0;  // mouse wheel notches this frame (up positive)
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;  // sticks -1..1 (dead zone applied), triggers 0..1
};
const Pad& pad_poll();  // once per frame
// kbm.cpp: keyboard and mouse as pad actions (F9 toggles photo mode); blanks the game's own keyboard/mouse
void kbm_init();   // hooks the game's DirectInput keyboard and mouse reads
void kbm_poll(Pad& pad, WORD prev_buttons, float dt);
bool kbm_last_used();  // the last input was keyboard or mouse (the hints show keys), else the pad
void kbm_pad_used();
extern volatile bool g_photo_mode;  // photo mode on: the game sees a neutral pad

// overlay.cpp
bool overlay_init();  // hooks IDXGISwapChain::Present

// camera.cpp
void camera_init();
void camera_frame(const Pad& pad, float dt);  // render thread, once per frame: the sticks

// ui.cpp: the panels and their buttons (render thread)
void ui_input(const Pad& pad, float dt);
void ui_draw(float pic_x, float pic_y, float pic_w, float pic_h);  // the game's picture, in pixels

// snap.cpp: Steam screenshot with the panels hidden
enum { SNAP_HIDE = 1, SNAP_CAPTURE = 2 };
void snap_request();
int snap_frame(float dt);   // render thread, each frame: SNAP_* flags for this frame
void snap_captured(const uint8_t* rgb, int w, int h);  // the frame's pixels (null: failed)
const char* snap_toast();   // a result to show for a moment, or null

// hud.cpp: the game's HUD, hidden while photo mode is on unless Y brings it back
void hud_init();
void hud_frame();   // render thread, each frame: applies the wanted state
void hud_toggle();  // Y
bool hud_shown();

// hint.cpp: the start hint (first run, or photo mode unopened for 30 days); records each opening
void hint_draw(float px, float py, float ph, float dt);  // render thread, each frame

// devchan.cpp (dev builds): file command channel for reverse engineering.
void devchan_start();
void devchan_frame();  // render thread: reapplies `hold` values

uintptr_t game_base();
uint8_t* find_pattern(const char* pat);  // camera.cpp: hex bytes, ?? wild, in the exe's first .text
uintptr_t* game_manager_ptr();         // camera.cpp: &GameManagerImp (null if not found)

// pose.cpp: the Pose panel's engine (plays the game's own animations on the player)
void pose_init();
void pose_frame(float dt);  // render thread, each frame (photo mode off: the character is given back)
bool pose_hooked();
int pose_count();
const char* pose_name(int i);
int pose_selected();             // 0: none
void pose_select(int i);         // plays it
float pose_frame_fraction();     // how far into the pose, 0..1
void pose_set_frame_fraction(float f);  // plays (or rewinds) to there, then holds
bool pose_frozen();
void pose_set_freeze(bool on);
float pose_turn();               // radians from where she faced
void pose_set_turn(float rad);
#ifdef PM_DEV
void pose_dev(const char* args, char* out, size_t n);  // devchan `pose ...`
#endif

// position.cpp: the Position panel's engine (moves and turns where the character is drawn;
// her physics stays put). Offsets in her own frame, from where she is; cleared when photo mode
// closes.
enum { POSN_X, POSN_Y, POSN_Z, POSN_PITCH, POSN_YAW, POSN_ROLL };
void position_init();
void position_frame(float dt);  // render thread, each frame
bool position_hooked();
float position_get(int axis);   // metres or radians
void position_set(int axis, float v);
void position_reset();
#ifdef PM_DEV
void position_dev(const char* args, char* out, size_t n);  // devchan `posn ...`
#endif

}  // namespace pm
