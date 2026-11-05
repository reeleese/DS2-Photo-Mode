// photo-mode.log, in the mod's own folder outside the game folder
// (%LOCALAPPDATA%\ds2-photo-mode\, under Proton inside the game's Wine prefix), so the
// mod adds nothing to the game folder besides its own files.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

namespace pm {

static wchar_t g_dir[MAX_PATH];
static CRITICAL_SECTION g_lock;
static FILE* g_file;

static void pick_dir() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) n = GetTempPathW(MAX_PATH, base);
    if (!n || n >= MAX_PATH - 32) { g_dir[0] = 0; return; }
    if (base[n - 1] == L'\\' || base[n - 1] == L'/') base[--n] = 0;
    _snwprintf(g_dir, MAX_PATH, L"%ls\\ds2-photo-mode\\", base);
    CreateDirectoryW(g_dir, nullptr);
}

void log_init(HMODULE) {
    InitializeCriticalSection(&g_lock);
    pick_dir();
    if (!g_dir[0]) return;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsphoto-mode.log", g_dir);
    g_file = _wfopen(path, L"w");
}

const wchar_t* data_dir() { return g_dir; }

void logf(const char* fmt, ...) {
    if (!g_file) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

}  // namespace pm
