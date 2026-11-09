#pragma once
// xLua hooks: game-thread execution for every IL2CPP call we make.
#include <windows.h>

DWORD WINAPI LuaHookThread(LPVOID);
DWORD WINAPI LuaConsoleThread(LPVOID);

// 1 once luaL_loadbufferx/lua_pcall are hooked. The stealth watcher re-runs
// LuaHookThread when a module loads and this is still 0 (xlua.dll was late).
extern volatile LONG g_luaHooked;
