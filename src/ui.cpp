// The panels (Camera, Camera XYZ, Pose, Body XYZ, Misc), bottom left inside the game's 16:9 picture, and
// their pad controls: D-pad up/down picks a row, left/right adjusts it (held: faster
// and faster), X switches panel, B puts the camera back on the game's.
#include "camera.h"
#include "imgui.h"
#include "uilayout.h"
#include <stdio.h>

namespace pm {

static const float DEG = 57.2957795f;

enum Kind { SLIDER, TOGGLE, CHOICE, PLACEHOLDER, ACTION };  // CHOICE: left/right steps through names(); ACTION: left/right does set()

struct Row {
    const char* label;
    Kind kind;
    float lo, hi;      // slider range, in display units
    bool log_scale;    // steps multiply (focal length, speed)
    bool wrap;         // yaw wraps round
    float tap;         // one press: added, or the factor for log rows
    float rate;        // held: display units per second (log rows: e-folds per second)
    const char* fmt;
    float (*get)(const CamState&);
    void (*set)(CamState&, float);
    int count;                    // CHOICE: how many values (get/set use 0..count-1)
    const char* (*names)(int);    // CHOICE: each value's name
};

static void move_to(CamState& c, Vec3 p) {
    if (c.orbit) c.pivot = c.pivot + (p - c.pos);
    c.pos = p;
}
static void orbit_follow(CamState& c) {
    if (c.orbit) c.pos = c.pivot - cam_forward(c) * c.orbit_dist;
}

static const Row CAMERA_ROWS[] = {
    {"Focal length", SLIDER, 8, 400, true, false, 1.05f, 0.45f, "%.0f mm",
     [](const CamState& c) { return fov_to_mm(c.fov); }, [](CamState& c, float v) { c.fov = mm_to_fov(v); }},
    {"Move speed", SLIDER, 0.1f, 30, true, false, 1.2f, 0.6f, "%.2f m/s",
     [](const CamState& c) { return c.speed; }, [](CamState& c, float v) { c.speed = v; }},
    {"Orbit around me", TOGGLE, 0, 1, false, false, 0, 0, nullptr,
     [](const CamState& c) { return c.orbit ? 1.0f : 0.0f; }, [](CamState&, float v) { camera_set_orbit(v > 0.5f); }},
};

static const Row FINE_ROWS[] = {
    {"X", SLIDER, -10000, 10000, false, false, 0.02f, 0.5f, "%.2f m",
     [](const CamState& c) { return c.pos.x; }, [](CamState& c, float v) { move_to(c, {v, c.pos.y, c.pos.z}); }},
    {"Y", SLIDER, -10000, 10000, false, false, 0.02f, 0.5f, "%.2f m",
     [](const CamState& c) { return c.pos.y; }, [](CamState& c, float v) { move_to(c, {c.pos.x, v, c.pos.z}); }},
    {"Z", SLIDER, -10000, 10000, false, false, 0.02f, 0.5f, "%.2f m",
     [](const CamState& c) { return c.pos.z; }, [](CamState& c, float v) { move_to(c, {c.pos.x, c.pos.y, v}); }},
    {"Pitch", SLIDER, -89, 89, false, false, 0.5f, 20, "%.1f\xc2\xb0",
     [](const CamState& c) { return c.pitch * DEG; }, [](CamState& c, float v) { c.pitch = v / DEG; orbit_follow(c); }},
    {"Yaw", SLIDER, -180, 180, false, true, 0.5f, 20, "%.1f\xc2\xb0",
     [](const CamState& c) { return c.yaw * DEG; }, [](CamState& c, float v) { c.yaw = v / DEG; orbit_follow(c); }},
    {"Roll", SLIDER, -180, 180, false, false, 0.5f, 20, "%.1f\xc2\xb0",
     [](const CamState& c) { return c.roll * DEG; }, [](CamState& c, float v) { c.roll = v / DEG; }},
};

static int g_grid;  // 0 none, 1 rule of thirds, 2 rule of halves
static const char* grid_name(int i) { return i == 1 ? "Rule of thirds" : i == 2 ? "Rule of halves" : "None"; }

static const Row MISC_ROWS[] = {
    {"Flight mode", CHOICE, 0, 0, false, false, 0, 0, nullptr,
     [](const CamState& c) { return (float)c.flight; }, [](CamState&, float v) { camera_set_flight((int)v); },
     FLIGHT_COUNT, flight_name},
    {"Grid", CHOICE, 0, 0, false, false, 0, 0, nullptr,
     [](const CamState&) { return (float)g_grid; }, [](CamState&, float v) { g_grid = (int)v; },
     3, grid_name},
    {"World freeze", PLACEHOLDER, 0, 1, false, false, 0, 0, "coming later", nullptr, nullptr, 0, nullptr},
};

// The character's pose (pose.cpp): the camera state isn't involved.
static const Row POSE_ROWS[] = {
    {"Pose", CHOICE, 0, 0, false, false, 0, 0, nullptr,
     [](const CamState&) { return (float)pose_selected(); }, [](CamState&, float v) { pose_select((int)v); },
     pose_count(), pose_name},
    {"Frame", SLIDER, 0, 100, false, false, 1, 25, "%.0f%%",
     [](const CamState&) { return pose_frame_fraction() * 100; }, [](CamState&, float v) { pose_set_frame_fraction(v / 100); }},
    {"Freeze", TOGGLE, 0, 1, false, false, 0, 0, nullptr,
     [](const CamState&) { return pose_frozen() ? 1.0f : 0.0f; }, [](CamState&, float v) { pose_set_freeze(v > 0.5f); }},
    {"Turn", SLIDER, -180, 180, false, true, 5, 60, "%.0f\xc2\xb0",
     [](const CamState&) { return pose_turn() * DEG; }, [](CamState&, float v) { pose_set_turn(v / DEG); }},
};
static const Row POSE_NA_ROWS[] = {
    {"Poses", PLACEHOLDER, 0, 1, false, false, 0, 0, "not found here", nullptr, nullptr, 0, nullptr},
};

// Where the character is drawn (position.cpp): offsets from where she stands, in her own frame
// (X her right, Y up, Z forward); the D-pad steps as on Camera XYZ. Cleared when photo mode closes.
#define POSN_ROW(label, axis, lo, hi, wrap, tap, rate, fmt, scale)                                                     \
    {label, SLIDER, lo, hi, false, wrap, tap, rate, fmt, [](const CamState&) { return position_get(axis) * (scale); }, \
     [](CamState&, float v) { position_set(axis, v / (scale)); }}
static const Row POSITION_ROWS[] = {
    POSN_ROW("X", POSN_X, -5, 5, false, 0.02f, 0.5f, "%.2f m", 1.0f),
    POSN_ROW("Y", POSN_Y, -5, 5, false, 0.02f, 0.5f, "%.2f m", 1.0f),
    POSN_ROW("Z", POSN_Z, -5, 5, false, 0.02f, 0.5f, "%.2f m", 1.0f),
    POSN_ROW("Pitch", POSN_PITCH, -180, 180, true, 0.5f, 20, "%.1f\xc2\xb0", DEG),
    POSN_ROW("Yaw", POSN_YAW, -180, 180, true, 0.5f, 20, "%.1f\xc2\xb0", DEG),
    POSN_ROW("Roll", POSN_ROLL, -180, 180, true, 0.5f, 20, "%.1f\xc2\xb0", DEG),
    {"Reset", ACTION, 0, 0, false, false, 0, 0, "all to 0", nullptr, [](CamState&, float) { position_reset(); }},
};
#undef POSN_ROW
static const Row POSITION_NA_ROWS[] = {
    {"Body XYZ", PLACEHOLDER, 0, 1, false, false, 0, 0, "not found here", nullptr, nullptr, 0, nullptr},
};

struct Panel { const char* name; const Row* rows; int n; };
static const Panel PANELS[] = {
    // Named for what each one moves or sets: the camera's lens, speed and orbit; the
    // camera's exact place; the character's pose; the character's place; flight mode and grid.
    {"Camera", CAMERA_ROWS, (int)(sizeof CAMERA_ROWS / sizeof CAMERA_ROWS[0])},
    {"Camera XYZ", FINE_ROWS, (int)(sizeof FINE_ROWS / sizeof FINE_ROWS[0])},
    {"Pose", POSE_ROWS, (int)(sizeof POSE_ROWS / sizeof POSE_ROWS[0])},
    {"Body XYZ", POSITION_ROWS, (int)(sizeof POSITION_ROWS / sizeof POSITION_ROWS[0])},
    {"Misc", MISC_ROWS, (int)(sizeof MISC_ROWS / sizeof MISC_ROWS[0])},
};
static const int NPANELS = 5;
static const int POSE_PANEL = 2, POSITION_PANEL = 3;
static const Panel POSE_NA = {"Pose", POSE_NA_ROWS, 1};
static const Panel POSITION_NA = {"Body XYZ", POSITION_NA_ROWS, 1};
// The panel as shown: the Pose and Body XYZ panels say so when their hooks aren't in (another
// game version).
static const Panel& panel_at(int i) {
    if (i == POSE_PANEL && !pose_hooked()) return POSE_NA;
    if (i == POSITION_PANEL && !position_hooked()) return POSITION_NA;
    return PANELS[i];
}

static int g_panel, g_row[NPANELS];
static float g_held;  // seconds left/right has been held

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void adjust(const Row& r, CamState& c, int dir, bool tap, float dt) {
    if (r.kind == PLACEHOLDER) return;
    if (r.kind == ACTION) {
        if (tap) r.set(c, 0);
        return;
    }
    if (r.kind == TOGGLE) {
        if (tap) r.set(c, r.get(c) > 0.5f ? 0.0f : 1.0f);
        return;
    }
    if (r.kind == CHOICE) {
        if (tap) r.set(c, (float)(((int)r.get(c) + dir + r.count) % r.count));
        return;
    }
    float v = r.get(c);
    float accel = g_held < 0.3f ? 0 : clampf(1 + (g_held - 0.3f), 1, 3);
    if (r.log_scale) v = tap ? (dir > 0 ? v * r.tap : v / r.tap) : v * expf(dir * r.rate * accel * dt);
    else v += tap ? dir * r.tap : dir * r.rate * accel * dt;
    if (r.wrap) {
        while (v > r.hi) v -= r.hi - r.lo;
        while (v < r.lo) v += r.hi - r.lo;
    } else {
        v = clampf(v, r.lo, r.hi);
    }
    r.set(c, v);
}

static bool g_overlay_hidden;  // Back / H: our panels and toast are not drawn (the grid stays)

void ui_input(const Pad& pad, float dt) {
    if (!g_photo_mode) {
        g_overlay_hidden = false;  // each photo mode session starts with the panels shown
        return;
    }
    if (pad.pressed & XINPUT_GAMEPAD_BACK) g_overlay_hidden = !g_overlay_hidden;
    camera_lock();
    CamState& c = camera_state();
    if (pad.pressed & XINPUT_GAMEPAD_X) g_panel = (g_panel + 1) % NPANELS;
    if (pad.pressed & XINPUT_GAMEPAD_B) camera_reset();
    if (pad.pressed & XINPUT_GAMEPAD_A) snap_request();
    if (pad.wheel != 0) {  // mouse wheel: focal length, a notch is about 8%
        float mm = fov_to_mm(c.fov) * powf(1.08f, pad.wheel);
        c.fov = mm_to_fov(clampf(mm, 8, 400));
    }
    // Y (HUD on/off) waits for the HUD option's address: see hud.cpp.
    const Panel& p = panel_at(g_panel);
    int& row = g_row[g_panel];
    if (row >= p.n) row = 0;
    if (pad.pressed & XINPUT_GAMEPAD_DPAD_UP) row = (row + p.n - 1) % p.n;
    if (pad.pressed & XINPUT_GAMEPAD_DPAD_DOWN) row = (row + 1) % p.n;
    int dir = ((pad.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) - ((pad.buttons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0);
    bool tap = pad.pressed & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_LEFT);
    if (dir) {
        adjust(p.rows[row], c, dir, tap, dt);
        g_held += dt;
    } else {
        g_held = 0;
    }
    camera_unlock();
}

// ---- drawing
static const ImU32 COL_TEXT = IM_COL32(236, 230, 214, 255);
static const ImU32 COL_DIM = IM_COL32(170, 162, 145, 255);
static const ImU32 COL_ACCENT = IM_COL32(222, 178, 92, 255);
static const ImU32 COL_SEL = IM_COL32(222, 178, 92, 60);
static const ImU32 COL_TRACK = IM_COL32(255, 255, 255, 40);

static float fraction(const Row& r, float v) {
    if (r.log_scale) return (::logf(v) - ::logf(r.lo)) / (::logf(r.hi) - ::logf(r.lo));  // pm::logf is the log file
    return (v - r.lo) / (r.hi - r.lo);
}

static const char* HELP[] = {"A snap   X cycle tabs   B reset", "D-pad pick/adjust   L3+R3 exit",
                             "LB/RB roll   LT/RT slower/faster", "LB/RB sink/rise   LT/RT slower/faster",
                             "LB/RB down/up   LT/RT slower/faster"};
// The same for keyboard and mouse (shown when that was the last input): mouse looks, WASD moves.
static const char* HELPK[] = {"Click snap   Tab tabs   R reset", "WASD move   Mouse look   F9 exit",
                              "Q/E roll   Ctrl/Shift slower/faster", "Q/E sink/rise   Ctrl/Shift slower/faster",
                              "Q/E down/up   Ctrl/Shift slower/faster"};
static const char* HELPK_ADJ = "Arrows pick/adjust   Wheel zoom";
static const char* HIDE_HELP = "Back hide panels";
static const char* HIDE_HELPK = "H hide panels";
static const float TAB_GAP = 18.0f, ROW_GAP = 28.0f;

// The widest any panel, value or help line can be (with the selected row's "< >" arrows), so
// the window never changes width. Cached per font size.
static float fixed_width() {
    static float cached, for_size;
    float size = ImGui::GetStyle().FontSizeBase;
    if (cached > 0 && for_size == size) return cached;
    WidthFit fit;
    float tabs[NPANELS];
    for (int i = 0; i < NPANELS; i++) tabs[i] = ImGui::CalcTextSize(PANELS[i].name).x;
    fit.need_tabs(tabs, NPANELS, TAB_GAP);
    auto rows = [&](const Panel& p) {
        for (int i = 0; i < p.n; i++) {
            const Row& r = p.rows[i];
            char buf[64], shown[80];
            auto value = [&](const char* v) {
                snprintf(shown, sizeof shown, "< %s >", v);
                fit.need_row(ImGui::CalcTextSize(r.label).x, ImGui::CalcTextSize(shown).x, ROW_GAP);
            };
            if (r.kind == CHOICE) {
                for (int k = 0; k < r.count; k++) value(r.names(k));
            } else if (r.kind == TOGGLE) {
                value("Off");
                value("On");
            } else if (r.kind == PLACEHOLDER || r.kind == ACTION) {
                value(r.fmt);
            } else {
                snprintf(buf, sizeof buf, r.fmt, r.lo);
                value(buf);
                snprintf(buf, sizeof buf, r.fmt, r.hi);
                value(buf);
                snprintf(buf, sizeof buf, r.fmt, -8888.88f);
                value(buf);
            }
        }
    };
    for (int i = 0; i < NPANELS; i++) rows(PANELS[i]);
    rows(POSE_NA);
    rows(POSITION_NA);
    ImGui::PushFont(nullptr, size * 0.8f);
    for (const char* h : HELP) fit.need(ImGui::CalcTextSize(h).x);
    for (const char* h : HELPK) fit.need(ImGui::CalcTextSize(h).x);
    fit.need(ImGui::CalcTextSize(HELPK_ADJ).x);
    fit.need(ImGui::CalcTextSize(HIDE_HELP).x);
    fit.need(ImGui::CalcTextSize(HIDE_HELPK).x);
    ImGui::PopFont();
    for_size = size;
    return cached = fit.w;
}

static void draw_row(const Row& r, const CamState& c, bool selected, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    bool track = r.kind == SLIDER && (r.hi - r.lo) < 5000;  // X/Y/Z have no useful range
    float h = lh + (track ? 10.0f : 4.0f);
    if (selected) dl->AddRectFilled(ImVec2(p0.x - 6, p0.y - 2), ImVec2(p0.x + width + 6, p0.y + h), COL_SEL, 4.0f);
    ImU32 tc = r.kind == PLACEHOLDER ? COL_DIM : COL_TEXT;
    dl->AddText(p0, tc, r.label);
    char buf[48];
    if (r.kind == TOGGLE) snprintf(buf, sizeof buf, "%s", r.get(c) > 0.5f ? "On" : "Off");
    else if (r.kind == CHOICE) snprintf(buf, sizeof buf, "%s", r.names((int)r.get(c)));
    else if (r.kind == PLACEHOLDER || r.kind == ACTION) snprintf(buf, sizeof buf, "%s", r.fmt);
    else snprintf(buf, sizeof buf, r.fmt, r.get(c));
    char shown[56];
    snprintf(shown, sizeof shown, selected && r.kind != PLACEHOLDER ? "< %s >" : "%s", buf);
    ImVec2 ts = ImGui::CalcTextSize(shown);
    dl->AddText(ImVec2(p0.x + width - ts.x, p0.y), selected && r.kind != PLACEHOLDER ? COL_ACCENT : tc, shown);
    if (track) {
        float f = clampf(fraction(r, r.get(c)), 0, 1);
        float y = p0.y + lh + 4;
        dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + width, y + 3), COL_TRACK, 1.5f);
        dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + width * f, y + 3), selected ? COL_ACCENT : COL_DIM, 1.5f);
    }
    ImGui::Dummy(ImVec2(width, h));
}

void ui_draw(float pic_x, float pic_y, float pic_w, float pic_h) {
    if (!g_photo_mode) return;
    if (g_grid) {  // over the picture only; ui_draw isn't called during a snap, so it's never in the shot
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        int n = g_grid == 1 ? 3 : 2;
        for (int i = 1; i < n; i++) {
            float x = pic_x + pic_w * i / n, y = pic_y + pic_h * i / n;
            bg->AddLine(ImVec2(x, pic_y), ImVec2(x, pic_y + pic_h), IM_COL32(255, 255, 255, 110), 1.5f);
            bg->AddLine(ImVec2(pic_x, y), ImVec2(pic_x + pic_w, y), IM_COL32(255, 255, 255, 110), 1.5f);
        }
    }
    if (g_overlay_hidden) return;  // nothing of ours but the grid
    const float margin = 20.0f;
    if (const char* t = snap_toast()) {  // after a snap: the result, where the panel's top was
        ImGui::SetNextWindowPos(ImVec2(pic_x + margin, pic_y + margin), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGuiWindowFlags tf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("Photo mode toast", nullptr, tf)) ImGui::TextUnformatted(t);
        ImGui::End();
    }
    ImGui::SetNextWindowPos(ImVec2(pic_x + margin, pic_y + pic_h - margin), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.7f);  // see-through, but readable over the HUD
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("Photo mode", nullptr, fl)) {
        const float width = fixed_width();
        if (!camera_hooked()) {
            ImGui::TextUnformatted("Photo mode: camera not found");
        } else {
            // Tabs: the active panel highlighted; X steps through them.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float x = p.x;
            for (int i = 0; i < NPANELS; i++) {
                ImVec2 ts = ImGui::CalcTextSize(PANELS[i].name);
                if (i == g_panel) {
                    dl->AddText(ImVec2(x, p.y), COL_ACCENT, PANELS[i].name);
                    dl->AddRectFilled(ImVec2(x, p.y + ts.y + 2), ImVec2(x + ts.x, p.y + ts.y + 4), COL_ACCENT);
                } else {
                    dl->AddText(ImVec2(x, p.y), COL_DIM, PANELS[i].name);
                }
                x += ts.x + TAB_GAP;
            }
            ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight() + 10));
            camera_lock();
            CamState c = camera_state();
            camera_unlock();
            const Panel& pn = panel_at(g_panel);
            for (int i = 0; i < pn.n; i++) draw_row(pn.rows[i], c, i == g_row[g_panel], width);
            ImGui::Dummy(ImVec2(width, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_DIM));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
            const char* const* help = kbm_last_used() ? HELPK : HELP;
            ImGui::TextUnformatted(help[0]);
            ImGui::TextUnformatted(help[1]);
            if (kbm_last_used()) ImGui::TextUnformatted(HELPK_ADJ);
            ImGui::TextUnformatted(c.flight == FLIGHT_6DOF ? help[2] : c.flight == FLIGHT_SEAMOTH ? help[3] : help[4]);
            ImGui::TextUnformatted(kbm_last_used() ? HIDE_HELPK : HIDE_HELP);
            ImGui::PopFont();
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

}  // namespace pm
