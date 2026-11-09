// Stella Sora Cheat — DLL entry point.
#include "features.h"
#include "il2cpp_api.h"
#include "overlay.h"
#include "lua_bridge.h"
#include "Stealth.h"
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        LoadConfig();

        // Hidden-thread path: scrub debug state, unlink from the PEB loader
        // lists, wipe our PE headers, then run the payload on threads that
        // never raise a debug event. If NtCreateThreadEx is unavailable the
        // fallback keeps the menu working, just visible.
        if (!stealth::Start(mod)) {
            CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);       // D3D11 hook + menu
            CreateThread(nullptr, 0, LuaHookThread, nullptr, 0, nullptr);    // xLua hooks
            CreateThread(nullptr, 0, LuaConsoleThread, nullptr, 0, nullptr); // diag + script queue
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        stealth::Stop();
    }
    return TRUE;
}
