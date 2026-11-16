#include "common.h"
#include "MinHook.h"

namespace pm {

static bool g_mh_ok;

uintptr_t game_base() { return (uintptr_t)GetModuleHandleW(nullptr); }

static DWORD WINAPI init_thread(void*) {
    if (!g_mh_ok) return 0;
    // These hook the game's code, which the protection unpacks at start: not under the loader
    // lock, and a moment later.
    Sleep(500);
    xinput_init();
    camera_init();
    pose_init();
    position_init();
    kbm_init();
#ifdef PM_DEV
    devchan_start();
#endif
    return 0;
}

}  // namespace pm

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        pm::log_init(inst);
        pm::logf("ds2 photo mode loaded, game base %p", (void*)pm::game_base());
        // The swap-chain hook goes in now, before the game's code runs and makes its factory
        // (only exports of dxgi.dll, which is loaded: the game imports it).
        pm::g_mh_ok = MH_Initialize() == MH_OK;
        if (pm::g_mh_ok) pm::overlay_init();
        else pm::logf("MH_Initialize failed");
        HANDLE t = CreateThread(nullptr, 0, pm::init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
