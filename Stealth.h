#pragma once
#include <windows.h>

// Anti-cheat stealth layer.
//
// Ported from the reconstructed loader (Stealth.cpp of the AsaNikke rebuild)
// and adapted to this payload:
//   - PEB loader-list unlink + PE header wipe  (module invisible to
//     EnumProcessModules / Toolhelp32 / PEB walks, no MZ at base)
//   - debug-state scrub (PEB->BeingDebugged, NtGlobalFlag, heap Flags/ForceFlags)
//   - ThreadHideFromDebugger on every thread, payload threads created with
//     THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER
//   - LdrRegisterDllNotification watcher (no kernelbase patch, no .text drift)
//   - UMPDC proxy import table so the module carries a power-management
//     footprint instead of nothing
//
// All ntdll lookups go through FNV-1a hashes; no resolved name sits in .rdata.
namespace stealth {

// Boots the layer and starts the payload threads. Call once from DllMain.
// Returns false if the hidden-thread path could not be established; the caller
// should then fall back to plain CreateThread so the menu still works.
bool Start(HMODULE self);

// PROCESS_DETACH: unregisters the loader notification and drops the proxy
// module. Nothing else is owned by the layer at that point.
void Stop();

} // namespace stealth
