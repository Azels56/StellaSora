// IL2CPP runtime access: exports, class/method lookup, invocation helpers.
#include "il2cpp_api.h"
#include "UnityResolve.hpp"
#include "str_obf.h"
#include "names.h"
#include <cstdio>
#include <cstring>
#include <vector>


using il2cpp_thread_attach_t = void* (*)(void*);
using il2cpp_assembly_get_image_t = void* (*)(void*);
using il2cpp_image_get_class_count_t = size_t (*)(void*);
using il2cpp_class_get_name_t = const char* (*)(void*);
using il2cpp_class_get_parent_t = void* (*)(void*);
using il2cpp_class_from_name_t = void* (*)(void*, const char*, const char*);
using il2cpp_runtime_invoke_t = void* (*)(void*, void*, void**, void**);
using il2cpp_field_get_offset_t = size_t (*)(void*);
using il2cpp_type_get_object_t = void* (*)(void*);
using il2cpp_array_element_size_t = int (*)(void*);


I2 g_i2;
void* g_domain = nullptr;
thread_local bool g_attached = false;

static void StartUnityResolve();

char          g_logRing[kLogRing][kLogLen];
volatile LONG g_logSeq = 0;

// Masked at compile time: a plaintext absolute path in .rdata identifies the
// payload, its purpose and its operator in one string scan.
static constexpr auto kLogPath = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\menu_log.txt");
static constexpr auto kLogDir  = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work");

void LogF(const char* fmt, ...) {
    char line[kLogLen];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    // The ring is the queue. Formatting and a lock-free store are all the game
    // thread pays for; the file write happens on the console thread.
    const LONG seq = InterlockedIncrement(&g_logSeq) - 1;
    strncpy_s(g_logRing[seq % kLogRing], kLogLen, line, _TRUNCATE);
}

// Drains whatever the ring holds. Single caller (LuaConsoleThread), so the
// cursor can live in a plain static. A torn line is possible if the writer laps
// the reader, which is cheaper than a lock on every log line.
void LogFlushPending() {
    static LONG flushed = 0;

    const LONG target = g_logSeq;
    LONG from = flushed;
    if (target == from) return;
    if (target - from > kLogRing) from = target - kLogRing;

    FILE* f = nullptr;
    fopen_s(&f, obf::Decoded(kLogPath), "a");
    if (!f) {
        CreateDirectoryA(obf::Decoded(kLogDir), nullptr);
        fopen_s(&f, obf::Decoded(kLogPath), "a");
        if (!f) return;
    }
    char line[kLogLen];
    for (LONG s = from; s < target; ++s) {
        strncpy_s(line, kLogLen, g_logRing[s % kLogRing], _TRUNCATE);
        fputs(line, f);
        fputc('\n', f);
    }
    fclose(f);
    flushed = target;
}

int LogTail(char* out, int outBytes, int maxLines) {
    if (!out || outBytes <= 0 || maxLines <= 0) return 0;
    out[0] = '\0';

    const LONG total = g_logSeq;
    if (total <= 0) return 0;

    const int avail = total < kLogRing ? (int)total : kLogRing;
    const int want  = maxLines < avail ? maxLines : avail;

    // Drop the oldest lines until the remainder fits the caller's buffer.
    int start = (int)total - want;
    for (;;) {
        int used = 0;
        for (int i = start; i < (int)total; ++i)
            used += (int)strlen(g_logRing[i % kLogRing]) + 1;
        if (used + 1 <= outBytes || start >= (int)total - 1) break;
        ++start;
    }

    char* w = out;
    int   n = 0;
    for (int i = start; i < (int)total; ++i) {
        const char*  s   = g_logRing[i % kLogRing];
        const size_t len = strlen(s);
        memcpy(w, s, len);
        w += len;
        *w++ = '\n';
        ++n;
    }
    *w = '\0';
    return n;
}

bool ResolveExports() {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return false;
#define RE(name) g_i2.name = (decltype(g_i2.name))GetProcAddress(ga, "il2cpp_" #name)
    RE(domain_get);
    RE(thread_attach);
    RE(domain_get_assemblies);
    RE(assembly_get_image);
    RE(image_get_name);
    RE(image_get_class_count);
    RE(image_get_class);
    RE(class_get_name);
    RE(class_get_namespace);
    RE(class_get_parent);
    RE(class_get_method_from_name);
    RE(class_from_name);
    RE(class_for_each);
    RE(runtime_invoke);
    RE(class_get_field_from_name);
    RE(field_get_offset);
    RE(class_get_type);
    RE(type_get_object);
    RE(object_get_class);
    RE(array_element_size);
    RE(string_new);
    RE(class_get_methods);
    RE(method_get_name);
    RE(method_get_param_count);
    RE(class_get_fields);
    RE(field_get_name);
    RE(field_get_type);
    RE(type_get_name);
    RE(method_get_param);
    RE(method_get_return_type);
#undef RE
    g_i2.ready = g_i2.domain_get && g_i2.thread_attach && g_i2.class_get_method_from_name && g_i2.runtime_invoke;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LogF("resolve ready=%d domain=%d attach=%d assemblies=%d image_name=%d class_name=%d method=%d invoke=%d field=%d type=%d array_elem=%d",
            (int)g_i2.ready, (int)(g_i2.domain_get != nullptr), (int)(g_i2.thread_attach != nullptr),
            (int)(g_i2.domain_get_assemblies != nullptr), (int)(g_i2.image_get_name != nullptr),
            (int)(g_i2.class_get_name != nullptr), (int)(g_i2.class_get_method_from_name != nullptr),
            (int)(g_i2.runtime_invoke != nullptr), (int)(g_i2.class_get_field_from_name != nullptr),
            (int)(g_i2.type_get_object != nullptr), (int)(g_i2.array_element_size != nullptr));
    }
    if (g_i2.ready) {
        g_domain = g_i2.domain_get();
        // No graph at boot: every gameplay lookup goes through the exports by
        // name, and only the script bridge's enumeration asks for the walk.
    }
    return g_i2.ready;
}

void EnsureThread() {
    if (!g_attached && g_i2.thread_attach && g_domain) {
        g_i2.thread_attach(g_domain);
        g_attached = true;
    }
}


// -------------------------------------------------------------- class lookup
// Field and method lookups run several times per actor per tick. Building a
// "%p:%s" key for each one costs a format pass plus a std::string allocation,
// so the key is kept as its parts instead. Equality still ends in strcmp, so a
// name buffer that gets reused at the same address cannot alias another entry.
struct MemberKey {
    void*       klass;
    const char* name;
    int         argc;
    bool operator==(const MemberKey& o) const {
        return klass == o.klass && argc == o.argc && std::strcmp(name, o.name) == 0;
    }
};

struct MemberKeyHash {
    size_t operator()(const MemberKey& k) const {
        size_t h = std::hash<void*>{}(k.klass);
        for (const char* p = k.name; p && *p; ++p) h = h * 131u + (size_t)(unsigned char)*p;
        return h ^ ((size_t)(unsigned)k.argc * 0x9E3779B1u);
    }
};

struct ClassCache {
    std::unordered_map<std::string, void*> classes;
    std::unordered_map<MemberKey, size_t, MemberKeyHash> fields;
    std::unordered_map<MemberKey, void*, MemberKeyHash> methods;
};

static ClassCache g_cache;

static std::unordered_map<std::string, void*> g_images;

void* ImageByName(const char* name) {
    auto it = g_images.find(name);
    if (it != g_images.end()) return it->second;
    void* found = nullptr;
    size_t asmCount = 0;
    void** asms = g_i2.domain_get_assemblies(g_domain, &asmCount);
    for (size_t a = 0; a < asmCount; a++) {
        void* image = g_i2.assembly_get_image(asms[a]);
        if (!image) continue;
        const char* n = g_i2.image_get_name(image);
        if (n && strcmp(n, name) == 0) { found = image; break; }
    }
    if (found) g_images[name] = found;
    return found;
}

// ------------------------------------------------------------ native lookup
// UnityResolve walks every class of GameAssembly to build its graph: several
// seconds and a large burst of allocations. Everything the gameplay code needs
// is reachable through the il2cpp exports directly, by name, so the graph is
// only built when the script bridge asks for enumeration. Resolving by name
// also means no class or method names have to be cached anywhere.
//
// The UnityResolve path stays as a fallback: it sees members the export-based
// lookup misses (generic instances, some interpreter types), and a miss there
// is worth the one-off cost.
static void* NativeFindClass(const char* name) {
    if (!g_domain || !g_i2.class_from_name || !g_i2.domain_get_assemblies || !g_i2.assembly_get_image)
        return nullptr;

    // "UnityEngine.Time" is split so the namespace is matched too: a bare name
    // picks whichever class is indexed first and the game ships many collisions.
    std::string full = name;
    std::string ns, cls = full;
    const size_t dot = full.rfind('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < full.size()) {
        ns  = full.substr(0, dot);
        cls = full.substr(dot + 1);
    }

    size_t n = 0;
    void** asms = g_i2.domain_get_assemblies(g_domain, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        void* img = g_i2.assembly_get_image(asms[i]);
        if (!img) continue;
        if (!ns.empty()) {
            if (void* c = g_i2.class_from_name(img, ns.c_str(), cls.c_str())) return c;
        }
        if (void* c = g_i2.class_from_name(img, "", name)) return c;
    }

    // Hotfix classes are registered under a namespace the caller does not know,
    // so a simple-name scan over the images is the only way to reach them. It
    // is two export calls per class, which is still far cheaper than building
    // the whole graph for one name.
    if (!g_i2.image_get_class_count || !g_i2.image_get_class || !g_i2.class_get_name) return nullptr;
    for (size_t i = 0; i < n; i++) {
        void* img = g_i2.assembly_get_image(asms[i]);
        if (!img) continue;
        const size_t count = g_i2.image_get_class_count(img);
        for (size_t k = 0; k < count; k++) {
            void* c = g_i2.image_get_class(img, k);
            if (!c) continue;
            const char* cn = g_i2.class_get_name(c);
            if (cn && strcmp(cn, cls.c_str()) == 0) return c;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------- UnityResolve
// The whole metadata graph (assemblies -> classes -> fields/methods) is built
// once by UnityResolve. It walks every class of GameAssembly, which takes long
// enough to drop frames, so it runs on a worker thread and callers see a null
// class until it is done.
static volatile LONG g_urState = 0;   // 0 idle, 1 building, 2 ready
static std::unordered_map<void*, UnityResolve::Class*> g_classByAddr;

static void BuildClassIndex() {
    g_classByAddr.reserve(65536);
    for (auto* as : UnityResolve::assembly)
        for (auto* c : as->classes)
            if (c && c->address) g_classByAddr.emplace(c->address, c);
}

static DWORD WINAPI UrInitThread(LPVOID) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) { InterlockedExchange(&g_urState, 0); return 0; }
    const ULONGLONG t0 = GetTickCount64();
    UnityResolve::Init(ga, UnityResolve::Mode::Il2Cpp);
    size_t classes = 0;
    for (auto* as : UnityResolve::assembly) classes += as->classes.size();
    BuildClassIndex();
    LogF("unityresolve: %zu assemblies, %zu classes, %llu ms",
         UnityResolve::assembly.size(), classes,
         (unsigned long long)(GetTickCount64() - t0));
    InterlockedExchange(&g_urState, 2);
    return 0;
}

static void StartUnityResolve() {
    if (InterlockedCompareExchange(&g_urState, 1, 0) != 0) return;
    HANDLE h = CreateThread(nullptr, 0, UrInitThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_urState, 0);
}

bool UrReady() { return g_urState == 2; }

// The script bridge enumerates classes and members, which the export-based
// lookups cannot answer. Opening the script tab is a deliberate act, so the
// full graph is built from there rather than on every launch.
void EnsureMetadataGraph() { if (!UrReady()) StartUnityResolve(); }

static UnityResolve::Class* ClassOf(void* klass) {
    auto it = g_classByAddr.find(klass);
    return it == g_classByAddr.end() ? nullptr : it->second;
}

// Both lookup tables cache their misses so the expensive name scan runs once.
// A miss taken while the metadata graph was still absent, however, hides
// members that only the graph can see - and the graph is built mid-session when
// the Script tab opens. Drop the negative entries once the fallback is live.
// Called only from the game thread, which is the sole user of these tables.
static bool g_missSwept = false;
static void SweepMissesOnce() {
    if (g_missSwept || !UrReady()) return;
    g_missSwept = true;
    for (auto it = g_cache.methods.begin(); it != g_cache.methods.end(); ) {
        if (!it->second) it = g_cache.methods.erase(it);
        else ++it;
    }
    for (auto it = g_cache.fields.begin(); it != g_cache.fields.end(); ) {
        if (it->second == SIZE_MAX) it = g_cache.fields.erase(it);
        else ++it;
    }
}

void* FindClassByName(const char* name) {
    if (!name) return nullptr;
    auto it = g_cache.classes.find(name);
    if (it != g_cache.classes.end()) return it->second;

    // The export lookup answers every name the payload uses without the graph,
    // which is what keeps the multi-second walk off a normal launch. A miss is
    // cached too: the simple-name scan is not free and must not repeat per tick.
    void* found = NativeFindClass(name);
    g_cache.classes[name] = found;
    if (found) LogF("class %s => %p", name, found);
    else LogF("class %s not found", name);
    return found;
}

size_t FieldOffset(void* klass, const char* field) {
    SweepMissesOnce();
    const MemberKey key{ klass, field, 0 };
    auto it = g_cache.fields.find(key);
    if (it != g_cache.fields.end()) return it->second;
    if (!klass || !field) return SIZE_MAX;

    // Walk the real parent chain instead of resolving parent names: a bare name
    // can match a different class when the game ships two with the same name,
    // and the field then silently reads as missing on every call.
    if (g_i2.class_get_field_from_name && g_i2.field_get_offset) {
        for (void* k = klass; k; ) {
            if (void* f = g_i2.class_get_field_from_name(k, field)) {
                const size_t off = g_i2.field_get_offset(f);
                g_cache.fields[key] = off;
                return off;
            }
            k = g_i2.class_get_parent ? g_i2.class_get_parent(k) : nullptr;
        }
    }
    if (UrReady()) {
        for (void* k = klass; k; ) {
            UnityResolve::Class* c = ClassOf(k);
            if (c) {
                for (auto* f : c->fields) {
                    if (f->name == field) {
                        const size_t off = (size_t)f->offset;
                        g_cache.fields[key] = off;
                        return off;
                    }
                }
            }
            k = g_i2.class_get_parent ? g_i2.class_get_parent(k) : nullptr;
        }
    }
    g_cache.fields[key] = SIZE_MAX;
    LogF("field %s missing on class %p", field, klass);
    return SIZE_MAX;
}

void* Method(void* klass, const char* name, int argc) {
    SweepMissesOnce();
    const MemberKey key{ klass, name, argc };
    auto it = g_cache.methods.find(key);
    if (it != g_cache.methods.end()) return it->second;
    if (!klass || !name) return nullptr;

    if (g_i2.class_get_method_from_name) {
        for (void* k = klass; k; ) {
            if (void* m = g_i2.class_get_method_from_name(k, name, argc)) {
                g_cache.methods[key] = m;
                return m;
            }
            k = g_i2.class_get_parent ? g_i2.class_get_parent(k) : nullptr;
        }
    }
    if (UrReady()) {
        for (void* k = klass; k; ) {
            UnityResolve::Class* c = ClassOf(k);
            if (c) {
                for (auto* m : c->methods) {
                    if (m->name == name && (int)m->args.size() == argc) {
                        g_cache.methods[key] = m->address;
                        return m->address;
                    }
                }
            }
            k = g_i2.class_get_parent ? g_i2.class_get_parent(k) : nullptr;
        }
    }
    g_cache.methods[key] = nullptr;
    LogF("method %s/%d missing on class %p", name, argc, klass);
    return nullptr;
}


// ------------------------------------------------------- UnityResolve handles
void* UrClass(void* klass) { return UrReady() ? (void*)ClassOf(klass) : nullptr; }

void* UrMethod(void* klass, const char* name, int argc) {
    if (!UrReady() || !name) return nullptr;
    for (void* k = klass; k; ) {
        UnityResolve::Class* c = ClassOf(k);
        if (!c) break;
        for (auto* m : c->methods)
            if (m->name == name && (int)m->args.size() == argc) return m;
        k = c->parent.empty() ? nullptr : FindClassByName(c->parent.c_str());
    }
    return nullptr;
}

void* UrField(void* klass, const char* name) {
    if (!UrReady() || !name) return nullptr;
    for (void* k = klass; k; ) {
        UnityResolve::Class* c = ClassOf(k);
        if (!c) break;
        for (auto* f : c->fields) if (f->name == name) return f;
        k = c->parent.empty() ? nullptr : FindClassByName(c->parent.c_str());
    }
    return nullptr;
}

bool Invoke(void* klass, const char* name, int argc, void* obj, void** params, int64_t* outInt, bool* outBool) {
    void* m = Method(klass, name, argc);
    if (!m) return false;
    void* exc = nullptr;
    void* ret = g_i2.runtime_invoke(m, obj, params, &exc);
    if (exc) return false;
    if (outInt) *outInt = ret ? *(int64_t*)((uint8_t*)ret + 0x10) : 0;
    if (outBool) *outBool = ret ? (*(uint8_t*)((uint8_t*)ret + 0x10)) != 0 : false;
    return true;
}


// -------------------------------------------------------------- object arrays
int ArrayLength(void* arr);

int ArrayLength(void* arr) {
    return arr ? *(int32_t*)((uint8_t*)arr + 0x18) : 0;
}


// ------------------------------------------------------------- stat plumbing
void* FieldPtr(void* obj, const char* field) {
    if (!obj) return nullptr;
    void* klass = g_i2.object_get_class(obj);
    size_t off = FieldOffset(klass, field);
    if (off == SIZE_MAX) return nullptr;
    return *(void**)((uint8_t*)obj + off);
}


bool Invoke8(void* klass, const char* name, void* obj, void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7) {
    void* m = Method(klass, name, 8);
    if (!m) return false;
    void* params[8] = { p0, p1, p2, p3, p4, p5, p6, p7 };
    void* exc = nullptr;
    g_i2.runtime_invoke(m, obj, params, &exc);
    return exc == nullptr;
}


void* InvokeStaticList(void* klass, const char* name, int argc, void** params) {
    void* m = Method(klass, name, argc);
    if (!m) return nullptr;
    void* exc = nullptr;
    void* ret = g_i2.runtime_invoke(m, nullptr, params, &exc);
    return exc ? nullptr : ret;
}

// Il2Cpp List<T>: _items @0x10, _size @0x18.
int ListSize(void* list) {
    if (!list) return 0;
    int n = *(int32_t*)((uint8_t*)list + 0x18);
    return (n < 0 || n > 4096) ? 0 : n;
}

void* ListItem(void* list, int i) {
    void* items = *(void**)((uint8_t*)list + 0x10);
    if (!items) return nullptr;
    return *(void**)((uint8_t*)items + 0x20 + (size_t)i * 8);
}


int64_t ListLong(void* list, int i) {
    void* items = *(void**)((uint8_t*)list + 0x10);
    if (!items) return 0;
    return *(int64_t*)((uint8_t*)items + 0x20 + (size_t)i * 8);
}

// Sweep by monster id: catches enemies that left the faction list or are
// immune to direct damage (the usual "one mob left" case).

// ---------------------------------------------------------------------------
// Runtime API probe.
//
// The shipping Hotfix.dll is encrypted on disk (CDPH container), so the only
// place the real type and method names exist is the loaded IL2CPP metadata.
// This walks the image and writes out everything whose name looks combat
// related, which is how the break / toughness / hit-count entry points are
// found.

static bool NameHas(const char* s, const char* const* keys, int n) {
    if (!s) return false;
    for (int i = 0; i < n; ++i)
        if (strstr(s, keys[i])) return true;
    return false;
}

// Three passes, all static (no live actors needed):
//   A  classes whose name looks combat related, with fields and methods
//   B  every field whose *type* is a toughness/shield type -> the owner classes
//   C  every method named like a damage entry point
void DumpHotfixApi(const char* path) {
    if (!UrReady()) return;

    static const char* const kClassKeys[] = {
        "Tough", "Break", "Shield", "Stagger", "Poise", "SuperArmor",
        "HitCount", "MultiHit", "Guard", "Armor", "Weaken"
    };
    static const char* const kFieldTypeKeys[] = { "MonsterToughnessInfo", "ActorShield" };
    static const char* const kMethodKeys[] = {
        "DamageTo", "BeHit", "TakeDamage", "ApplyDamage", "OnDamage",
        "DamageActor", "HitDamage", "DamageTarget", "DoDamage"
    };
    static const char* const kTargets[] = {
        names::ILRuntimeAPI(), names::ActorEffectManage(), names::MonsterActor(), names::AdventureActor()
    };

    auto has = [](const std::string& s, const char* const* keys, size_t n) {
        for (size_t i = 0; i < n; ++i) if (s.find(keys[i]) != std::string::npos) return true;
        return false;
    };

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    fprintf(f, "domain=%p unityresolve=%d\n", g_domain, (int)UrReady());

    int classes = 0, matched = 0, typeHits = 0, methodHits = 0;
    for (auto* as : UnityResolve::assembly) {
        if (as->name.find("Hotfix") == std::string::npos) continue;
        fprintf(f, "image %s classes=%zu\n", as->name.c_str(), as->classes.size());

        for (auto* c : as->classes) {
            ++classes;
            const std::string& kn = c->name;

            // B: fields whose *type* is a toughness / shield component
            for (auto* fld : c->fields) {
                const std::string tn = fld->type ? fld->type->name : "";
                if (!has(tn, kFieldTypeKeys, 2)) continue;
                fprintf(f, "[B] %s.%s : %s\n", kn.c_str(), fld->name.c_str(), tn.c_str());
                ++typeHits;
            }

            // D: explicit target classes, with signatures and entry pointers
            bool target = false;
            for (const char* t : kTargets) if (kn == t) target = true;
            if (target) {
                fprintf(f, "\n[D] class %s\n", kn.c_str());
                for (auto* m : c->methods) {
                    const bool named = m->name.find("Damage") != std::string::npos ||
                                       m->name.find("Hit") != std::string::npos ||
                                       m->name.find("Shield") != std::string::npos ||
                                       m->name.find("Tough") != std::string::npos;
                    if (!named && !has(m->name, kMethodKeys, 9)) continue;
                    std::string sig;
                    for (size_t i = 0; i < m->args.size(); ++i) {
                        if (i) sig += ",";
                        sig += m->args[i] && m->args[i]->pType ? m->args[i]->pType->name : "?";
                    }
                    fprintf(f, "    m %s/%zu ptr=%p (%s)\n", m->name.c_str(),
                            m->args.size(), m->function, sig.c_str());
                }
            }

            // A: class name match, dump everything
            if (has(kn, kClassKeys, sizeof(kClassKeys) / sizeof(kClassKeys[0]))) {
                ++matched;
                fprintf(f, "\n[A] class %s%s%s\n", c->namespaze.empty() ? "" : c->namespaze.c_str(),
                        c->namespaze.empty() ? "" : ".", kn.c_str());
                for (auto* fld : c->fields) fprintf(f, "    f %s\n", fld->name.c_str());
                for (auto* m : c->methods) fprintf(f, "    m %s/%zu\n", m->name.c_str(), m->args.size());
            }

            // C: damage entry points anywhere
            for (auto* m : c->methods) {
                if (!has(m->name, kMethodKeys, sizeof(kMethodKeys) / sizeof(kMethodKeys[0]))) continue;
                fprintf(f, "[C] %s.%s/%zu\n", kn.c_str(), m->name.c_str(), m->args.size());
                ++methodHits;
            }
        }
    }
    fprintf(f, "\nscanned=%d classMatched=%d typeFields=%d damageMethods=%d\n",
            classes, matched, typeHits, methodHits);
    fclose(f);
    LogF("hotfix api dump: %d classes, %d class hits, %d type fields, %d damage methods",
         classes, matched, typeHits, methodHits);
}
