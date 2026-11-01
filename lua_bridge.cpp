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
static std::mutex g_luaMutex;
static std::string g_luaPending;
static int g_luaRuns = 0;
static ULONGLONG g_lastAuraTick = 0;

static __declspec(noinline) int RunLuaCodeSEH(void* L, const char* code) {
    __try {
        int top = g_lua_gettop(L);
        if (g_luaL_loadstring(L, code) != 0) { g_lua_settop(L, top); return -2; }
        int rc = g_lua_pcall(L, 0, 0, 0);
        g_lua_settop(L, top);
        return rc;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

static void QueueLua(const std::string& code) {
    std::lock_guard<std::mutex> lock(g_luaMutex);
    g_luaPending = code;
}

void QueueDiag() {
    QueueLua(
        "local out={} local function P(k,v) local s=k..'='..tostring(v); out[#out+1]=s end "
        "local ok,pl=pcall(function() return CS.AdventureBehaviour.ILRuntimeAPI.GetAllPlayerActorAndMinion(false) end) P('pl_ok',ok) P('pl_n',pl and pl.Count) "
        "if ok and pl then for i=0,pl.Count-1 do local a=pl[i]; P('act'..i, tostring(a)..'|id='..tostring(a and a.Id)) end end "
        "P('scene', pcall(function() return CS.UnityEngine.SceneManagement.SceneManager.GetActiveScene().name end)) "
        "local okm,m=pcall(function() return CS.AdventureModuleHelper.GetCurrentMonsters() end) P('mon_ok',okm) P('mon_n',m and m.Count) "
        "local okd,d=pcall(function() return CS.AdventureModuleHelper.GetCurrentDynamicActors() end) P('dyn_ok',okd) P('dyn_n',d and d.Count) "
        "if ok and pl and pl.Count>0 then "
        "local okf,f=pcall(function() return CS.AdventureBehaviour.ILRuntimeAPI.GetConfrontFactionActors(pl[0], false) end) P('foe_ok',okf) P('foe_n',f and f.Count) "
        "if okf and f then for i=0,math.min(f.Count,5)-1 do P('foe'..i,tostring(f[i])) end end end "
        "if ok and pl then for i=0,pl.Count-1 do local a=pl[i]; local id=a and a.Id; if id and id>0 then "
        "local okc,cd=pcall(function() return CS.AdventureModuleHelper.GetPlayerSkillCd(id) end) P('cd'..id,tostring(okc)..':'..tostring(cd)) "
        "if okc and cd then P('energy'..id, tostring(pcall(function() return cd:GetEnergy() end))) P('total'..id, tostring(pcall(function() return cd:GetTotalEnergy() end))) end end end end "
        "local okf,f=pcall(function() return io.open([[arena_diag.txt]],'w') end) "
        "if okf and f then pcall(function() f:write(table.concat(out,'\\n')) f:close() end) end");
}

static int __cdecl hkLuaPCall(void* L, int nargs, int nres, int errf) {
    if (InterlockedCompareExchange(&g_luaBusy, 1, 0) == 0) {
        std::string code;
        {
            std::lock_guard<std::mutex> lock(g_luaMutex);
            if (!g_luaPending.empty()) { code.swap(g_luaPending); }
        }
        MaybeRunCheats();
        if (!code.empty() && g_luaL_loadstring && g_lua_gettop && g_lua_settop) {
            int rc = RunLuaCodeSEH(L, code.c_str());
            if (rc == 0) g_luaRuns++;
            else LogF("lua snippet rc=%d", rc);
        }
        InterlockedExchange(&g_luaBusy, 0);
    }
    return g_lua_pcall(L, nargs, nres, errf);
}

DWORD WINAPI LuaConsoleThread(LPVOID) {
    for (;;) {
        const char* path = "lua_run.lua";
        static ULONGLONG lastDiag = 0;
        if (GetTickCount64() - lastDiag >= 10000) {
            lastDiag = GetTickCount64();
            QueueDiag();
        }
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
    InitializeCriticalSection(&g_luaLock);
    g_luaLockInit = true;
    for (int i = 0; i < 600; i++) {
        HMODULE xl = GetModuleHandleA("xlua.dll");
        if (xl) {
            void* fn = (void*)GetProcAddress(xl, "luaL_loadbufferx");
            if (fn && MH_CreateHook(fn, (void*)&hkLuaLLoadBufferX, (void**)&g_luaL_loadbufferx) == MH_OK) {
                MH_EnableHook(fn);
                g_lua_pcall = (lua_pcall_t)GetProcAddress(xl, "lua_pcall");
                g_luaL_loadstring = (luaL_loadstring_t)GetProcAddress(xl, "luaL_loadstring");
                g_lua_gettop = (lua_gettop_t)GetProcAddress(xl, "lua_gettop");
                g_lua_settop = (lua_settop_t)GetProcAddress(xl, "lua_settop");
                if (g_lua_pcall && g_luaL_loadstring && g_lua_gettop && g_lua_settop) {
                    void* pc = (void*)g_lua_pcall;
                    if (MH_CreateHook(pc, (void*)&hkLuaPCall, (void**)&g_lua_pcall) == MH_OK) MH_EnableHook(pc);
                } else {
                    LogF("lua console exports missing");
                }
                LogF("lua dump hook installed");
                return 0;
            }
        }
        Sleep(100);
    }
    LogF("lua dump hook failed");
    return 0;
}
