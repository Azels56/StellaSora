# Stella Sora Cheat

by Azels56 — native IL2CPP menu for Stella Sora (Windows x64).

## Layout

```
tools/menu/
  StellaSoraCheat.sln        Visual Studio 2022 solution
  StellaSoraCheat.vcxproj    the DLL (stella_menu.dll)
  StellaSoraInjector.vcxproj the launcher/injector EXE
  src/main.cpp               DLL entry point (threads)
  src/il2cpp_api.h/.cpp      IL2CPP exports + class/method helpers
  src/features.h/.cpp        cheats: attributes, skills, aura, config
  src/overlay.h/.cpp         D3D11 hook, theme, menu, damage numbers
  src/lua_bridge.h/.cpp      xLua hooks (game-thread execution)
  injector/main.cpp          suspended-process injector with path prompt
  build.bat                  one-shot DLL build (cl)
  build_injector.bat         one-shot injector build (cl)
  build_vs.bat               msbuild the whole solution
  bin/Release/               stella_menu.dll + StellaSoraInjector.exe
  ../../imgui, ../../minhook vendored dependencies
```

## Build

* Visual Studio: open `StellaSoraCheat.sln`, select `Release | x64`, Build.
* Command line: `build_vs.bat` (both projects) or `build.bat` (DLL only).

## Run

Double-click `run+inject.bat` in the project root, or run:

```
bin\Release\StellaSoraInjector.exe "C:\YostarGames\StellaSora_EN\StellaSora.exe" "..\stella_menu.dll"
```

The injector prompts for the game path when started without arguments, closes a
running instance if you agree, launches it **suspended**, injects, then resumes.
This is required: ACE refuses `LoadLibrary` once the game has started.

Press `INSERT` in game for the menu.

## Features

* Combat — Damage / Defense multipliers, Game Speed (1–5x), Attack / Move Speed
* Player — God Mode (also multiplies Max HP), Always Critical Hit
* Skills — No Skill Cooldown, Unlimited Energy, Unlimited Dash, Kill Aura (+ one-shot)
* Safety — ACE presence, inline-patch status, Safe Mode
* Config — wallpaper/backdrop opacity, UI scale, save/reload

## Safety notes

* ACE (`ACE-Base64.dll`) is loaded in the game process and supervises the
  launcher's Frida gadget services. The menu does not use Frida.
* **Safe Mode** (default ON) keeps everything in managed data: no inline code
  patches and no GM orders. Turning it off enables the `CanUseCD` hook and the
  GM sweep used for enemies that stay immune.
* Injected at process creation only — late injection is blocked.
* Windows Defender may flag/quarantine the built DLL (PUP heuristic). Add an
  exclusion for the project folder before building/testing.
* Settings persist in `..\..\work\stella_menu.ini` (window bounds, toggles,
  sliders, appearance).

## Credits and disclaimer

See the main [README](../../README.md) for full credits (Hiro420,
Taiga74164, Aragon12UC, monoxgas, Dear ImGui, MinHook, Il2CppDumper, ILSpy) and the
disclaimer.

Stella Sora and all of its original assets belong to Yostar Games; all credits go to
their rightful owners. This is a fan-made educational project, not affiliated with or
endorsed by Yostar Games. I am not liable for any damages caused if you get banned from
using a mod created by this tool, or its derivatives. I DO NOT CLAIM ANY RESPONSIBILITY
FOR ANY USAGE OF THIS SOFTWARE, THE SOFTWARE IS MADE 100% FOR EDUCATIONAL PURPOSES ONLY.
