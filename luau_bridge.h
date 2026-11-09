#pragma once
// Standalone Luau VM bound to the live IL2CPP metadata (see luau_bridge.cpp).

// Polls work\luau_run.lua and runs it. Must be called from the game thread.
void LuauTick();

// Runs a chunk immediately. Returns false and fills err on a compile/runtime
// error. Safe to call before the metadata graph is ready.
bool LuauRun(const char* code, char* err, int errCap);

// Hands a chunk to the poller above. Called from the render thread, which must
// not invoke IL2CPP itself, so the write is the whole job here.
bool LuauQueue(const char* code);

// One-line result of the last queued run, for the menu footer.
const char* LuauStatus();

// Abandons the VM after a fault unwound through the C bridge. The Lua state is
// half-torn at that point and reusing it faults again, so the next run builds a
// fresh one instead.
void LuauAbandon();
