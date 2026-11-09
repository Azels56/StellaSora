#pragma once
// D3D11 present hook, ImGui theme and menu.
#include <windows.h>

struct ImFont;

DWORD WINAPI InitThread(LPVOID);
DWORD WINAPI HotkeyThread(LPVOID);
void ShowDamageNumber(void* target, long long damage);

// Menu visibility. Owned here, drawn by Menu.cpp.
extern bool g_menuOpen;

// Set on the first Present frame. Rendering means the player finished booting,
// which is the earliest safe moment to unlink the payload from the loader list.
extern volatile LONG g_overlayBooted;

// Segoe UI Bold baked at 28px, for titles and section headers. Null when the
// face is missing; callers fall back to the regular font.
extern ImFont* g_fontBold;

// Width / height of the loaded wallpaper, so the menu can cover-fit it instead
// of stretching it. Zero until the texture is loaded.
extern float g_wallAspect;
