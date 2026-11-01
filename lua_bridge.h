#pragma once
// xLua hooks: game-thread execution for every IL2CPP call we make.
#include <windows.h>

void QueueDiag();
DWORD WINAPI LuaHookThread(LPVOID);
DWORD WINAPI LuaConsoleThread(LPVOID);
