#include "common.h"
#include "MinHook.h"

namespace pm {

uintptr_t game_base() { return (uintptr_t)GetModuleHandleW(nullptr); }

static DWORD WINAPI init_thread(void*) {
    // The game creates its D3D device after the loader is done; hooking Present from a
    // throwaway swap chain can happen any time, but not under the loader lock.
    Sleep(500);
    xinput_init();
    if (MH_Initialize() != MH_OK) {
        logf("MH_Initialize failed");
        return 0;
    }
    camera_init();
    pose_init();
    position_init();
    kbm_init();
    overlay_init();
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
        HANDLE t = CreateThread(nullptr, 0, pm::init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
