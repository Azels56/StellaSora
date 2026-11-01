// Stella Sora Cheat — DLL entry point.
#include "features.h"
#include "il2cpp_api.h"
#include "overlay.h"
#include "lua_bridge.h"
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        LoadConfig();
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);       // D3D11 hook + menu
        CreateThread(nullptr, 0, LuaHookThread, nullptr, 0, nullptr);    // xLua hooks
        CreateThread(nullptr, 0, LuaConsoleThread, nullptr, 0, nullptr); // diag + script queue
    }
    return TRUE;
}
