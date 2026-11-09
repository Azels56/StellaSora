// xLua hooks. Every IL2CPP call runs on the game thread from here.
#include "lua_bridge.h"
#include "features.h"
#include "il2cpp_api.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <mutex>
#include <cstdint>
#include "MinHook.h"

typedef int(__cdecl* luaL_loadbufferx_t)(void*, const char*, size_t, const char*, const char*);
typedef int(__cdecl* lua_pcall_t)(void*, int, int, int);
typedef int(__cdecl* luaL_loadstring_t)(void*, const char*);
typedef int(__cdecl* lua_gettop_t)(void*);
typedef void(__cdecl* lua_settop_t)(void*, int);


static luaL_loadbufferx_t g_luaL_loadbufferx = nullptr;
static int g_luaChunks = 0;
static CRITICAL_SECTION g_luaLock;
static bool g_luaLockInit = false;

static void DumpLuaChunk(const char* buff, size_t size, const char* name) {
    if (!buff || size == 0 || size > 64 * 1024 * 1024) return;
    return; // dumping disabled: file IO on the game thread caused load stalls
    if (!g_luaLockInit) return;
    CreateDirectoryA("lua_dump", nullptr); // fix
    char path[512];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
        "%05d_%.120s.luac",
        InterlockedIncrement((volatile LONG*)&g_luaChunks), (name && *name) ? name : "chunk");
    for (char* p = path; *p; ++p) { if (*p == ':' || *p == '?' || *p == '*' || *p == '"' || *p == '<' || *p == '>' || *p == '|' || *p == '/' || *p == '\n' || *p == '\r') *p = '_'; }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(buff, 1, size, f); fclose(f); }
}

static int __cdecl hkLuaLLoadBufferX(void* L, const char* buff, size_t size, const char* name, const char* mode) {
    DumpLuaChunk(buff, size, name);
    return g_luaL_loadbufferx(L, buff, size, name, mode);
}


typedef int(__cdecl* lua_pcall_t)(void*, int, int, int);
typedef int(__cdecl* luaL_loadstring_t)(void*, const char*);
typedef int(__cdecl* lua_gettop_t)(void*);
typedef void(__cdecl* lua_settop_t)(void*, int);
static lua_pcall_t g_lua_pcall = nullptr;
static luaL_loadstring_t g_luaL_loadstring = nullptr;
static lua_gettop_t g_lua_gettop = nullptr;
static lua_settop_t g_lua_settop = nullptr;
static volatile LONG g_luaBusy = 0;
volatile LONG g_luaHooked = 0;
static std::mutex g_luaMutex;
static std::string g_luaPending;
static int g_luaRuns = 0;
static ULONGLONG g_lastAuraTick = 0;

// ------------------------------------------------------------- embedded chunk
// Loaded into the game's own state once it is alive. ss_multi / ss_log are C
// closures registered with pushcclosure + setglobal so the script can read
// live feature state without a string round-trip.
bool g_luaScriptLoaded = false;
static ULONGLONG g_luaEmbedNextTry = 0;

typedef int(__cdecl* lua_CFunction)(void*);
typedef void(__cdecl* lua_pushcclosure_t)(void*, lua_CFunction, int);
typedef void(__cdecl* lua_setglobal_t)(void*, const char*);
typedef void(__cdecl* lua_pushinteger_t)(void*, long long);
typedef void(__cdecl* lua_pushboolean_t)(void*, int);
typedef const char* (__cdecl* lua_tolstring_t)(void*, int, size_t*);
typedef int(__cdecl* lua_getglobal_t)(void*, const char*);
typedef int(__cdecl* lua_type_t)(void*, int);

static lua_pushcclosure_t g_lua_pushcclosure = nullptr;
static lua_setglobal_t g_lua_setglobal = nullptr;
static lua_pushinteger_t g_lua_pushinteger = nullptr;
static lua_pushboolean_t g_lua_pushboolean = nullptr;
static lua_tolstring_t g_lua_tolstring = nullptr;
static lua_getglobal_t g_lua_getglobal = nullptr;
static lua_type_t g_lua_type = nullptr;

// Multi-hit lives in features.cpp: combat calls HitActors from C#, so wrapping
// the xLua binding table here never saw a call. Only the small read-only
// globals are still registered, for console snippets.
//
// ss_auto_tick is the story/card driver. The AVG (visual novel) system is
// scripted in Lua and its skip is a plain uGUI button: btnSkip raises a confirm
// popup, btnConfirm1 accepts it. The event channel (EventId.AvgSkip) is only a
// notification the ctrl fires after that button is pressed - hitting it does
// nothing on its own, which is why the first version looked dead.
//
// The menu is folded while reading, and btnSkip is inactive until it unfolds,
// so the tick walks: confirm -> skip -> unfold, whichever is available.
//
// The fate card panel (VampireFateCardSelectPanel) does not use btnConfirm. Its
// confirm is btnConfirmReward, a full-screen 2160x1350 NaviButton - that single
// button is also what "select anywhere to continue" listens on, so clicking it
// covers both the confirm and the tap-anywhere prompt.
static const char kEmbedLua[] =
    "if ss_log then ss_log('stella lua bridge ready') end\n"
    "local t_skip, t_pick = 0, 0\n"
    "local pickLatch = false\n"
    "local sawCards = false\n"
    "local idx, idxAt = nil, 0\n"
    "local function build()\n"
    "  local ok, arr = pcall(function()\n"
    "    return CS.UnityEngine.Resources.FindObjectsOfTypeAll(typeof(CS.UnityEngine.UI.Button))\n"
    "  end)\n"
    "  if not ok or not arr then return nil end\n"
    "  local t = {}\n"
    "  for i = 0, arr.Length - 1 do\n"
    "    local b = arr[i]\n"
    "    if b then\n"
    "      local ok2, nm, act = pcall(function()\n"
    "        local g = b.gameObject\n"
    "        return g.name, g.activeInHierarchy\n"
    "      end)\n"
    "      if ok2 and act and nm and not t[nm] then t[nm] = b end\n"
    "    end\n"
    "  end\n"
    "  return t\n"
    "end\n"
    "local function index()\n"
    "  local now = os.clock()\n"
    "  if idx and now - idxAt < 0.3 then return idx end\n"
    "  idx = build(); idxAt = now\n"
    "  return idx\n"
    "end\n"
    "local function press(t, want)\n"
    "  local b = t[want]\n"
    "  if b then pcall(function() b.onClick:Invoke() end); return true end\n"
    "  return false\n"
    "end\n"
    "local function press_prefix(t, prefix)\n"
    "  for nm, b in pairs(t) do\n"
    "    if nm:sub(1, #prefix) == prefix then\n"
    "      pcall(function() b.onClick:Invoke() end)\n"
    "      return true\n"
    "    end\n"
    "  end\n"
    "  return false\n"
    "end\n"
    "function ss_auto_tick(skip, pick)\n"
    "  local now = os.clock()\n"
    "  local doSkip = skip and now - t_skip >= 0.5\n"
    "  local doPick = pick and now - t_pick >= 0.6\n"
    "  if not doSkip and not doPick then return end\n"
    "  local t = index()\n"
    "  if not t then return end\n"
    "  local avg = t['btnSpeedUp'] or t['btnUIOnOff'] or t['btnSkip'] or t['btnFoldMenu']\n"
    "  if doSkip then\n"
    "    t_skip = now\n"
    "    if avg then\n"
    "      if not press(t, 'btnConfirm1') and not press(t, 'btnSkip') then\n"
    "        press(t, 'btnFoldMenu')\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if doPick then\n"
    "    t_pick = now\n"
    "    local panel = t['btnFateCard1'] or t['btnFateCard2'] or t['btnFateCard3']\n"
    "                  or t['btnAutoSelectClose']\n"
    "    if not panel then sawCards = false end\n"
    "    if pickLatch then\n"
    "      pickLatch = false\n"
    "      if panel then\n"
    "        sawCards = false\n"
    "        if not press(t, 'btnConfirmReward') then press(t, 'btnConfirm') end\n"
    "      end\n"
    "    elseif panel and (press(t, 'btnFateCard1') or press(t, 'btnFateCard2')\n"
    "                   or press(t, 'btnFateCard3')) then\n"
    "      pickLatch = true\n"
    "      sawCards = true\n"
    "    elseif panel and sawCards then\n"
    "      sawCards = false\n"
    "      if not press(t, 'btnConfirmReward') then press(t, 'btnConfirm') end\n"
    "    elseif not panel and avg then\n"
    "      press_prefix(t, 'Choice_')\n"
    "    end\n"
    "  end\n"
    "end\n";

static int __cdecl LuaSSMulti(void* L) {
    if (g_lua_pushinteger) g_lua_pushinteger(L, (long long)g_cheat.multiHit);
    return 1;
}

static int __cdecl LuaSSLog(void* L) {
    size_t n = 0;
    const char* s = g_lua_tolstring ? g_lua_tolstring(L, 1, &n) : nullptr;
    if (s) LogF("lua: %.120s", s);
    return 0;
}

static bool InstallEmbeddedLua(void* L) {
    if (!g_luaL_loadbufferx || !g_lua_pcall || !g_lua_gettop || !g_lua_settop) return false;
    if (!g_lua_pushcclosure || !g_lua_setglobal || !g_lua_pushinteger) return false;

    const int top = g_lua_gettop(L);
    g_lua_pushcclosure(L, (lua_CFunction)&LuaSSMulti, 0);
    g_lua_setglobal(L, "ss_multi");
    g_lua_pushcclosure(L, (lua_CFunction)&LuaSSLog, 0);
    g_lua_setglobal(L, "ss_log");

    int rc = g_luaL_loadbufferx(L, kEmbedLua, sizeof(kEmbedLua) - 1, "@stella_embed", "t");
    if (rc == 0) rc = g_lua_pcall(L, 0, 0, 0);
    if (rc != 0) {
        size_t n = 0;
        const char* err = g_lua_tolstring ? g_lua_tolstring(L, -1, &n) : nullptr;
        LogF("embedded lua failed rc=%d %s", rc, err ? err : "?");
        g_lua_settop(L, top);
        return false;
    }
    g_lua_settop(L, top);
    LogF("embedded lua loaded (%d bytes)", (int)(sizeof(kEmbedLua) - 1));
    return true;
}

// A bad chunk must not take the game down with it.
static bool InstallEmbeddedLuaSEH(void* L) {
    __try { return InstallEmbeddedLua(L); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogF("embedded lua raised 0x%08lX", GetExceptionCode());
        return false;
    }
}

// The game runs more than one Lua state and tears states down between scenes,
// so "installed once" is not the same as "installed here". The chunk leaves the
// ss_multi global behind, which makes a cheap liveness check per pcall.
static bool EmbeddedLuaPresent(void* L) {
    if (!g_lua_getglobal || !g_lua_type || !g_lua_gettop || !g_lua_settop) return false;
    const int top = g_lua_gettop(L);
    g_lua_getglobal(L, "ss_multi");
    const int t = g_lua_type(L, -1);
    g_lua_settop(L, top);
    return t == 6; // LUA_TFUNCTION
}

// Drives the story/card toggles through the chunk installed above. Called from
// the game thread once per pcall; the chunk owns its own rate limits, so this
// is two pushes and a call when the toggles are off.
static void RunAutoTick(void* L) {
    if (!g_cheat.autoSkipStory && !g_cheat.autoSelectCards) return;
    if (!g_lua_getglobal || !g_lua_pushboolean || !g_lua_type || !g_lua_pcall) return;

    const int top = g_lua_gettop(L);
    g_lua_getglobal(L, "ss_auto_tick");
    if (g_lua_type(L, -1) == 6) { // LUA_TFUNCTION
        g_lua_pushboolean(L, g_cheat.autoSkipStory ? 1 : 0);
        g_lua_pushboolean(L, g_cheat.autoSelectCards ? 1 : 0);
        g_lua_pcall(L, 2, 0, 0);
    }
    g_lua_settop(L, top);
}

static __declspec(noinline) int RunLuaCodeSEH(void* L, const char* code) {    __try {
        int top = g_lua_gettop(L);
        if (g_luaL_loadstring(L, code) != 0) { g_lua_settop(L, top); return -2; }
        int rc = g_lua_pcall(L, 0, 0, 0);
        g_lua_settop(L, top);
        return rc;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

// Everything the tick touches is game-side state. A fault in any of it used to
// escape hkLuaPCall with g_luaBusy still latched at 1, which silently turned
// every feature off for the rest of the session. The body is kept free of C++
// objects so it can carry its own SEH frame: a __try inside hkLuaPCall would be
// rejected outright, because the string and lock_guard there need unwinding.
static void __declspec(noinline) CheatTickBody(void* L, const char* code) {
    RunPendingAction();
    MaybeRunCheats();
    if (code && *code && g_luaL_loadstring && g_lua_gettop && g_lua_settop) {
        const int rc = RunLuaCodeSEH(L, code);
        if (rc == 0) g_luaRuns++;
        else LogF("lua snippet rc=%d", rc);
    }
    if (!EmbeddedLuaPresent(L)) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_luaEmbedNextTry) {
            if (InstallEmbeddedLuaSEH(L)) {
                g_luaScriptLoaded = true;
                LogF("embedded lua installed on state %p", L);
            } else {
                g_luaEmbedNextTry = now + 2000;
            }
        }
    } else {
        g_luaScriptLoaded = true;
    }
    // Story/card automation runs here, after the embedded chunk is known to be
    // in this state, and before the game's own pcall proceeds.
    RunAutoTick(L);
}

static void CheatTick(void* L, const char* code) {
    __try { CheatTickBody(L, code); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogF("cheat tick faulted 0x%08lX", GetExceptionCode());
    }
}

static int __cdecl hkLuaPCall(void* L, int nargs, int nres, int errf) {
    if (InterlockedCompareExchange(&g_luaBusy, 1, 0) == 0) {
        std::string code;
        {
            std::lock_guard<std::mutex> lock(g_luaMutex);
            if (!g_luaPending.empty()) { code.swap(g_luaPending); }
        }
        // Drain an overlay action before the cheat pass, so a "Reset Stats"
        // click lands and stays landed instead of being re-applied the same
        // tick. This is the game thread; the action bar is not.
        CheatTick(L, code.empty() ? nullptr : code.c_str());
        // Released unconditionally: CheatTick swallows its own faults, so this
        // is the one place the re-entrancy guard can never be left behind.
        InterlockedExchange(&g_luaBusy, 0);
    }
    return g_lua_pcall(L, nargs, nres, errf);
}

DWORD WINAPI LuaConsoleThread(LPVOID) {
    for (;;) {
        const char* path = "lua_run.lua";

        // Also the log sink: LogF only queues into the ring, and this thread is
        // the one place that touches a file handle for it.
        LogFlushPending();

        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            FILE* f = nullptr;
            if (fopen_s(&f, path, "rb") == 0 && f) {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                if (n > 0 && n < 1024 * 1024) {
                    std::string code; code.resize((size_t)n);
                    fread(&code[0], 1, (size_t)n, f);
                    {
                        std::lock_guard<std::mutex> lock(g_luaMutex);
                        g_luaPending = code;
                    }
                }
                fclose(f);
            }
            DeleteFileA(path);
        }
        Sleep(400);
    }
    return 0;
}


DWORD WINAPI LuaHookThread(LPVOID) {
    // 0 = untouched, 1 = hooked, 2 = hooking in progress. The stealth watcher
    // may re-spawn this thread when a module loads; only one may run.
    if (InterlockedCompareExchange(&g_luaHooked, 2, 0) != 0) return 0;
    InitializeCriticalSection(&g_luaLock);
    g_luaLockInit = true;
    MH_STATUS lastStatus = MH_OK;
    for (int i = 0; i < 1800; i++) {
        HMODULE xl = GetModuleHandleA("xlua.dll");
        if (xl) {
            void* fn = (void*)GetProcAddress(xl, "luaL_loadbufferx");
            if (!fn) {
                lastStatus = MH_ERROR_NOT_INITIALIZED; // export missing, not MH's fault
            } else {
                lastStatus = MH_CreateHook(fn, (void*)&hkLuaLLoadBufferX, (void**)&g_luaL_loadbufferx);
            }
            if (fn && lastStatus == MH_OK) {
                MH_EnableHook(fn);
                g_lua_pcall = (lua_pcall_t)GetProcAddress(xl, "lua_pcall");
                g_luaL_loadstring = (luaL_loadstring_t)GetProcAddress(xl, "luaL_loadstring");
                g_lua_gettop = (lua_gettop_t)GetProcAddress(xl, "lua_gettop");
                g_lua_settop = (lua_settop_t)GetProcAddress(xl, "lua_settop");
                g_lua_pushcclosure = (lua_pushcclosure_t)GetProcAddress(xl, "lua_pushcclosure");
                g_lua_setglobal = (lua_setglobal_t)GetProcAddress(xl, "lua_setglobal");
                g_lua_pushinteger = (lua_pushinteger_t)GetProcAddress(xl, "lua_pushinteger");
                g_lua_pushboolean = (lua_pushboolean_t)GetProcAddress(xl, "lua_pushboolean");
                g_lua_tolstring = (lua_tolstring_t)GetProcAddress(xl, "lua_tolstring");
                g_lua_getglobal = (lua_getglobal_t)GetProcAddress(xl, "lua_getglobal");
                g_lua_type = (lua_type_t)GetProcAddress(xl, "lua_type");
                if (g_lua_pcall && g_luaL_loadstring && g_lua_gettop && g_lua_settop) {
                    void* pc = (void*)g_lua_pcall;
                    if (MH_CreateHook(pc, (void*)&hkLuaPCall, (void**)&g_lua_pcall) == MH_OK) MH_EnableHook(pc);
                } else {
                    LogF("lua console exports missing");
                }
                LogF("lua dump hook installed");
                InterlockedExchange(&g_luaHooked, 1);
                return 0;
            }
        }
        Sleep(100);
    }
    LogF("lua dump hook failed (last MH status %d, xlua=%p)", (int)lastStatus,
         (void*)GetModuleHandleA("xlua.dll"));
    InterlockedExchange(&g_luaHooked, 0); // let the watcher retry on next load
    return 0;
}
