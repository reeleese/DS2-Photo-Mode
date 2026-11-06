// The "Photo mode: L3+R3" start hint: shown for the first seconds of a game, only on the
// first run (no photo-mode.ini beside the DLL) or when photo mode was last opened more than
// 30 days ago. Each time photo mode opens, the date goes into photo-mode.ini ([hint]
// last_opened=YYYY-MM-DD), written as a temp file and renamed. A missing or unreadable file
// is treated as a first run; a failed write is ignored.
#include "common.h"
#include "hintmath.h"
#include "imgui.h"
#include <stdio.h>

namespace pm {

static wchar_t g_ini[MAX_PATH], g_tmp[MAX_PATH];
static bool g_loaded, g_due, g_was_on;
static float g_shown;   // seconds since the first frame
static bool g_hide;     // photo mode opened this session

static long today_days(SYSTEMTIME* st) {
    GetLocalTime(st);
    return hint_days(st->wYear, st->wMonth, st->wDay);
}

static void load() {
    g_loaded = true;
    HMODULE self = nullptr;
    wchar_t dir[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&load, &self)) {
        g_due = true;  // nowhere to keep it: show the hint, write nothing
        return;
    }
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) { g_due = true; return; }
    wchar_t* slash = wcsrchr(dir, L'\\');
    wchar_t* fwd = wcsrchr(dir, L'/');
    if (fwd > slash) slash = fwd;
    if (!slash) { g_due = true; return; }
    slash[1] = 0;
    _snwprintf(g_ini, MAX_PATH, L"%lsphoto-mode.ini", dir);
    _snwprintf(g_tmp, MAX_PATH, L"%lsphoto-mode.ini.tmp", dir);

    char buf[512];
    size_t len = 0;
    if (FILE* f = _wfopen(g_ini, L"rb")) {
        len = fread(buf, 1, sizeof buf, f);
        fclose(f);
    }
    long last = 0;
    SYSTEMTIME st;
    long today = today_days(&st);
    g_due = hint_due(hint_parse(buf, len, &last), last, today);
}

static void write_today() {
    if (!g_ini[0]) return;
    SYSTEMTIME st;
    today_days(&st);
    char text[32];
    hint_format(text, st.wYear, st.wMonth, st.wDay);
    FILE* f = _wfopen(g_tmp, L"wb");
    if (!f) return;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(g_tmp, g_ini, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(g_tmp);
        logf("photo-mode.ini not written");
    }
}

void hint_draw(float px, float py, float ph, float dt) {
    if (!g_loaded) load();
    if (g_photo_mode && !g_was_on) {
        g_hide = true;
        write_today();
    }
    g_was_on = g_photo_mode;
    g_shown += dt;
    if (!g_due || g_hide || g_shown > 15.0f) return;
    ImGui::SetNextWindowPos(ImVec2(px + 20, py + ph - 20), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("Photo mode hint", nullptr, fl)) ImGui::TextUnformatted("Photo mode: L3+R3");
    ImGui::End();
}

}  // namespace pm
