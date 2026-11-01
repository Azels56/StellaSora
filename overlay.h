#pragma once
// D3D11 present hook, ImGui theme and menu.
#include <windows.h>

DWORD WINAPI InitThread(LPVOID);
void ShowDamageNumber(void* target, long long damage);
