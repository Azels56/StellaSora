// A standalone Luau VM over the UnityResolve metadata graph.
//
// Deliberately independent of the game's own xLua state: scripts here cannot
// corrupt combat Lua, and the VM survives scene and stage transitions. The
// bridge exposes just enough of the type system to poke actors by hand.
#include "luau_bridge.h"
#include "il2cpp_api.h"
#include "UnityResolve.hpp"
#include "str_obf.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

static lua_State* g_L = nullptr;
static bool       g_LuauFailed = false;
static char       g_status[192] = "idle";

const char* LuauStatus() { return g_status; }

// A script that faults inside the C bridge - a bad ss_read_i64 offset, say -
// unwinds past Lua's own cleanup, so the state is left half-torn. Closing it
// can fault again, so the pointer is simply dropped and the next run builds a
// new VM. The leak is one state per fault, which is the cheap side of the deal.
void LuauAbandon() {
    if (!g_L) return;
    g_L = nullptr;
    LogF("luau: vm abandoned after fault");
}

static constexpr auto kRunPath  = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\luau_run.lua");
static constexpr auto kTempPath = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\luau_run.tmp");

static const char* RunPath()  { static const auto d = obf::Decoded(kRunPath);  return d; }
static const char* TempPath() { static const auto d = obf::Decoded(kTempPath); return d; }

// ------------------------------------------------------------------ helpers
static bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

static int ParamWidth(const std::string& t) {
    if (t.find("Int64") != std::string::npos || t.find("UInt64") != std::string::npos ||
        t.find("Double") != std::string::npos || EndsWith(t, "FP")) return 8;
    if (t.find("Int32") != std::string::npos || t.find("UInt32") != std::string::npos ||
        t.find("Single") != std::string::npos) return 4;
    if (t.find("Int16") != std::string::npos) return 2;
    if (t.find("Boolean") != std::string::npos || t.find("Byte") != std::string::npos) return 1;
    return 8; // reference type
}

// ------------------------------------------------------------------- bridge
static int L_log(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    LogF("luau: %.150s", s ? s : "");
    return 0;
}

static int L_ready(lua_State* L) {
    // Enumeration needs the graph, so asking for readiness is what starts it.
    EnsureMetadataGraph();
    lua_pushboolean(L, UrReady() ? 1 : 0);
    return 1;
}

static int L_find_class(lua_State* L) {
    void* k = FindClassByName(luaL_checkstring(L, 1));
    if (!k) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, k);
    return 1;
}

static int L_method(lua_State* L) {
    void* k = lua_touserdata(L, 1);
    const char* n = luaL_checkstring(L, 2);
    const int argc = (int)luaL_optinteger(L, 3, -1);
    if (!k || !n) { lua_pushnil(L); return 1; }

    UnityResolve::Class* c = (UnityResolve::Class*)UrClass(k);
    if (!c) { lua_pushnil(L); return 1; }
    for (auto* m : c->methods) {
        if (m->name != n) continue;
        if (argc >= 0 && (int)m->args.size() != argc) continue;
        lua_pushlightuserdata(L, m);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// args(method) -> { "TypeName", ... }. The managed signature is the only way
// to tell an FDP long from a plain number before invoking anything.
static int L_args(lua_State* L) {
    UnityResolve::Method* m = (UnityResolve::Method*)lua_touserdata(L, 1);
    lua_newtable(L);
    if (!m) return 1;
    int n = 0;
    for (auto* a : m->args) {
        lua_pushstring(L, (a && a->pType) ? a->pType->name.c_str() : "?");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// methods(class[, substring]) -> { "Name/argc", ... }
//
// Without this the bridge is unusable from a script: a name alone does not say
// how many parameters the overload takes, and ss_method rejects the wrong arity.
static int L_methods(lua_State* L) {
    void* k = lua_touserdata(L, 1);
    const char* filter = luaL_optstring(L, 2, nullptr);
    lua_newtable(L);

    UnityResolve::Class* c = k ? (UnityResolve::Class*)UrClass(k) : nullptr;
    if (!c) return 1;

    int n = 0;
    for (auto* m : c->methods) {
        if (filter && *filter && m->name.find(filter) == std::string::npos) continue;
        char buf[256];
        _snprintf_s(buf, _TRUNCATE, "%s/%d", m->name.c_str(), (int)m->args.size());
        lua_pushstring(L, buf);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// fields(class[, substring]) -> { "Name", ... }
static int L_fields(lua_State* L) {
    void* k = lua_touserdata(L, 1);
    const char* filter = luaL_optstring(L, 2, nullptr);
    lua_newtable(L);

    UnityResolve::Class* c = k ? (UnityResolve::Class*)UrClass(k) : nullptr;
    if (!c) return 1;

    int n = 0;
    for (auto* f : c->fields) {
        if (filter && *filter && f->name.find(filter) == std::string::npos) continue;
        lua_pushstring(L, f->name.c_str());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int L_field(lua_State* L) {
    void* k = lua_touserdata(L, 1);
    void* f = k ? UrField(k, luaL_checkstring(L, 2)) : nullptr;
    if (!f) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, f);
    return 1;
}

static int L_offset(lua_State* L) {
    auto* f = (UnityResolve::Field*)lua_touserdata(L, 1);
    lua_pushinteger(L, f ? f->offset : -1);
    return 1;
}

// invoke(method, obj|nil, ...) -> unboxed result
static int L_invoke(lua_State* L) {
    auto* m = (UnityResolve::Method*)lua_touserdata(L, 1);
    if (!m || !m->address) { lua_pushnil(L); return 1; }

    void* obj = lua_isnoneornil(L, 2) ? nullptr : lua_touserdata(L, 2);

    const int n = (int)m->args.size();
    // runtime_invoke is handed `n` as the argument count. The staging arrays
    // are fixed at 16, so a wider signature would have made the interpreter
    // read past params[] and pass whatever followed on the stack. Refuse the
    // call instead of silently truncating it.
    if (n > 16) {
        luaL_error(L, "invoke: %d parameters, limit is 16", n);
        lua_pushnil(L);
        return 1;
    }
    int64_t  wide[16] = { 0 };
    int32_t  narrow[16] = { 0 };
    float    real[16] = { 0 };
    void*    strs[16] = { 0 };
    void*    params[16] = { 0 };

    for (int i = 0; i < n && i < 16; ++i) {
        const int li = i + 3;
        const std::string tn = (m->args[i] && m->args[i]->pType) ? m->args[i]->pType->name : "";
        if (lua_isnoneornil(L, li)) { params[i] = nullptr; continue; }

        if (lua_type(L, li) == LUA_TLIGHTUSERDATA) {
            wide[i] = (int64_t)(uintptr_t)lua_touserdata(L, li);
            params[i] = &wide[i];
            continue;
        }
        if (lua_type(L, li) == LUA_TSTRING && tn.find("String") != std::string::npos) {
            const char* s = lua_tostring(L, li);
            strs[i] = g_i2.string_new ? g_i2.string_new(s ? s : "") : nullptr;
            params[i] = &strs[i];
            continue;
        }
        if (lua_type(L, li) == LUA_TBOOLEAN) {
            narrow[i] = lua_toboolean(L, li) ? 1 : 0;
            params[i] = &narrow[i];
            continue;
        }

        const int w = ParamWidth(tn);
        if (w == 4 && tn.find("Single") != std::string::npos) {
            real[i] = (float)lua_tonumber(L, li);
            params[i] = &real[i];
        } else if (w == 4 || w == 1 || w == 2) {
            narrow[i] = (int32_t)lua_tonumber(L, li);
            params[i] = &narrow[i];
        } else {
            wide[i] = (int64_t)lua_tonumber(L, li);
            params[i] = &wide[i];
        }
    }

    void* exc = nullptr;
    void* ret = g_i2.runtime_invoke ? g_i2.runtime_invoke(m->address, obj, n ? params : nullptr, &exc) : nullptr;
    if (exc) { lua_pushnil(L); return 1; }

    const std::string rt = m->return_type ? m->return_type->name : "System.Void";
    if (rt.find("Void") != std::string::npos) { lua_pushnil(L); return 1; }
    if (!ret) { lua_pushnil(L); return 1; }

    const uint8_t* box = (const uint8_t*)ret + 0x10;
    if (rt.find("Boolean") != std::string::npos) { lua_pushboolean(L, *box ? 1 : 0); return 1; }
    if (rt.find("Single") != std::string::npos) { lua_pushnumber(L, *(const float*)box); return 1; }
    if (rt.find("Double") != std::string::npos) { lua_pushnumber(L, *(const double*)box); return 1; }
    if (rt.find("Int32") != std::string::npos) { lua_pushnumber(L, *(const int32_t*)box); return 1; }
    if (rt.find("Int64") != std::string::npos || EndsWith(rt, "FP")) {
        lua_pushnumber(L, (double)*(const int64_t*)box);
        return 1;
    }
    if (rt.find("String") != std::string::npos) {
        // Il2CppString: length at +0x10, UTF-16 payload at +0x14.
        const int32_t len = *(const int32_t*)box;
        const uint16_t* w = (const uint16_t*)(box + 4);
        char tmp[512];
        int j = 0;
        for (int32_t i = 0; i < len && j < (int)sizeof(tmp) - 1; ++i) {
            const uint16_t c = w[i];
            tmp[j++] = (c < 128) ? (char)c : '?';
        }
        tmp[j] = 0;
        lua_pushstring(L, tmp);
        return 1;
    }
    lua_pushlightuserdata(L, ret);
    return 1;
}

static int L_read_i64(lua_State* L) {
    const uintptr_t p = (uintptr_t)lua_touserdata(L, 1) + (uintptr_t)luaL_checkinteger(L, 2);
    lua_pushnumber(L, (double)*(const int64_t*)p);
    return 1;
}

static int L_write_i64(lua_State* L) {
    const uintptr_t p = (uintptr_t)lua_touserdata(L, 1) + (uintptr_t)luaL_checkinteger(L, 2);
    *(int64_t*)p = (int64_t)luaL_checknumber(L, 3);
    return 0;
}

static int L_read_i32(lua_State* L) {
    const uintptr_t p = (uintptr_t)lua_touserdata(L, 1) + (uintptr_t)luaL_checkinteger(L, 2);
    lua_pushnumber(L, *(const int32_t*)p);
    return 1;
}

static int L_write_i32(lua_State* L) {
    const uintptr_t p = (uintptr_t)lua_touserdata(L, 1) + (uintptr_t)luaL_checkinteger(L, 2);
    *(int32_t*)p = (int32_t)luaL_checknumber(L, 3);
    return 0;
}

static int L_class_of(lua_State* L) {
    void* o = lua_touserdata(L, 1);
    if (!o || !g_i2.object_get_class) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, g_i2.object_get_class(o));
    return 1;
}

static void RegisterBridge(lua_State* L) {
    lua_pushcfunction(L, L_log, "log");              lua_setglobal(L, "ss_log");
    lua_pushcfunction(L, L_ready, "ready");          lua_setglobal(L, "ss_ready");
    lua_pushcfunction(L, L_find_class, "find_class");lua_setglobal(L, "ss_class");
    lua_pushcfunction(L, L_method, "method");        lua_setglobal(L, "ss_method");
    lua_pushcfunction(L, L_methods, "methods");      lua_setglobal(L, "ss_methods");
    lua_pushcfunction(L, L_args, "args");            lua_setglobal(L, "ss_args");
    lua_pushcfunction(L, L_fields, "fields");        lua_setglobal(L, "ss_fields");
    lua_pushcfunction(L, L_field, "field");          lua_setglobal(L, "ss_field");
    lua_pushcfunction(L, L_offset, "offset");        lua_setglobal(L, "ss_offset");
    lua_pushcfunction(L, L_invoke, "invoke");        lua_setglobal(L, "ss_invoke");
    lua_pushcfunction(L, L_read_i64, "read_i64");    lua_setglobal(L, "ss_read_i64");
    lua_pushcfunction(L, L_write_i64, "write_i64");  lua_setglobal(L, "ss_write_i64");
    lua_pushcfunction(L, L_read_i32, "read_i32");    lua_setglobal(L, "ss_read_i32");
    lua_pushcfunction(L, L_write_i32, "write_i32");  lua_setglobal(L, "ss_write_i32");
    lua_pushcfunction(L, L_class_of, "class_of");    lua_setglobal(L, "ss_class_of");
}

static lua_State* VM() {
    if (g_L || g_LuauFailed) return g_L;
    g_L = luaL_newstate();
    if (!g_L) { g_LuauFailed = true; return nullptr; }
    luaL_openlibs(g_L);
    RegisterBridge(g_L);
    LogF("luau vm ready");
    return g_L;
}

bool LuauRun(const char* code, char* err, int errCap) {
    if (err && errCap > 0) err[0] = 0;
    lua_State* L = VM();
    if (!L) { if (err) _snprintf_s(err, errCap, _TRUNCATE, "vm unavailable"); return false; }
    if (!code) return true;

    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t bcSize = 0;
    char* bc = luau_compile(code, strlen(code), &opts, &bcSize);
    if (!bc) { if (err) _snprintf_s(err, errCap, _TRUNCATE, "compile failed"); return false; }

    const int load = luau_load(L, "@stella", bc, bcSize, 0);
    free(bc);
    if (load != 0) {
        const char* m = lua_tostring(L, -1);
        if (err) _snprintf_s(err, errCap, _TRUNCATE, "%s", m ? m : "load error");
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char* m = lua_tostring(L, -1);
        if (err) _snprintf_s(err, errCap, _TRUNCATE, "%s", m ? m : "runtime error");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool LuauQueue(const char* code) {
    if (!code) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, TempPath(), "wb") != 0 || !f) {
        _snprintf_s(g_status, _TRUNCATE, "cannot write script file");
        return false;
    }
    const size_t n = strlen(code);
    fwrite(code, 1, n, f);
    fclose(f);

    // Move, never write in place: the poller runs on the game thread and would
    // otherwise be able to pick up a half-written chunk.
    if (!MoveFileExA(TempPath(), RunPath(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(TempPath());
        _snprintf_s(g_status, _TRUNCATE, "queue failed (%lu)", GetLastError());
        return false;
    }
    _snprintf_s(g_status, _TRUNCATE, "queued \xc2\xb7 %d bytes", (int)n);
    return true;
}

void LuauTick() {
    static ULONGLONG lastCheck = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastCheck < 400) return;
    lastCheck = now;
    if (GetFileAttributesA(RunPath()) == INVALID_FILE_ATTRIBUTES) return;

    std::string code;
    FILE* f = nullptr;
    if (fopen_s(&f, RunPath(), "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 512 * 1024) {
            code.resize((size_t)n);
            fread(&code[0], 1, (size_t)n, f);
        }
        fclose(f);
    }
    DeleteFileA(RunPath());
    if (code.empty()) return;

    char err[256];
    if (LuauRun(code.c_str(), err, sizeof(err))) {
        _snprintf_s(g_status, _TRUNCATE, "ok \xc2\xb7 %d bytes", (int)code.size());
        LogF("luau: script ok (%d bytes)", (int)code.size());
    }
    else {
        _snprintf_s(g_status, _TRUNCATE, "error \xc2\xb7 %.120s", err);
        LogF("luau: %s", err);
    }
}
