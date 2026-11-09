#pragma once
// Ported ASA menu shell: header band, left tab rail, sectioned cards, action
// bar, status bar, wallpaper backdrop.
#include <imgui.h>

namespace menu {

void ApplyTheme();

// wallpaper may be null (falls back to a flat backdrop).
void Draw(ImTextureID wallpaper, int wallpaperAlpha, int dimAlpha);

// Forces the visible tab. Only the offscreen preview host uses this.
void SetTab(int tab);

// True while the open/close fade is still in flight. The overlay has to keep
// presenting during it; otherwise it early-outs on "menu closed" and the fade
// never runs, so the menu pops in and out at full alpha.
bool Animating();

} // namespace menu
