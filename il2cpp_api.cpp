// IL2CPP runtime access: exports, class/method lookup, invocation helpers.
#include "il2cpp_api.h"
#include <cstdio>


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

void LogF(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, "C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\menu_log.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
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
struct ClassCache {
    std::unordered_map<std::string, void*> classes;
    std::unordered_map<std::string, size_t> fields;
    std::unordered_map<std::string, void*> methods;
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

static int g_enumCount = 0;
static void ClassEnumCb(void* klass, void* user) {
    g_enumCount++;
    void** slots = (void**)user;
    const char* target = (const char*)slots[0];
    if (!klass || !target) return;
    const char* cn = g_i2.class_get_name(klass);
    if (cn && strcmp(cn, target) == 0) {
        *(void**)slots[1] = klass;
    }
}

void* FindClassByName(const char* name) {
    auto it = g_cache.classes.find(name);
    if (it != g_cache.classes.end()) return it->second;
    if (!g_i2.ready) return nullptr;
    EnsureThread();
    if (g_i2.class_for_each) {
        const char* target = name;
        void* found = nullptr;
        void* ud[2] = { (void*)target, &found };
        g_i2.class_for_each(ClassEnumCb, ud);
        if (found) {
            g_cache.classes[name] = found;
            LogF("class %s enum => %p", name, found);
            return found;
        }
    }
    static bool loggedImages = false;
    if (!loggedImages) {
        loggedImages = true;
        size_t asmCount = 0;
        void** asms = g_i2.domain_get_assemblies(g_domain, &asmCount);
        for (size_t a = 0; a < asmCount; a++) {
            void* image = g_i2.assembly_get_image(asms[a]);
            const char* n = image ? g_i2.image_get_name(image) : nullptr;
            if (n && (strstr(n, "Hotfix") || strstr(n, "Game") || strstr(n, "CoreModule"))) LogF("image: %s", n);
        }
    }
    struct Cand { const char* image; const char* ns; };
    static const Cand cands[] = {
        { "Hotfix.dll", "" },
        { "Hotfix.dll", "Nova.Hotfix" },
        { "Game.dll", "" },
        { "Assembly-CSharp.dll", "" },
        { "UnityEngine.CoreModule.dll", "UnityEngine" },
        { "mscorlib.dll", "System" },
        { "mscorlib.dll", "" },
    };
    for (const auto& c : cands) {
        void* image = ImageByName(c.image);
        if (!image || !g_i2.class_from_name) continue;
        void* k = g_i2.class_from_name(image, c.ns, name);
        if (k) {
            g_cache.classes[name] = k;
            LogF("class %s found in %s ns='%s' => %p", name, c.image, c.ns, k);
            return k;
        }
    }
    // fallback: scan every image
    size_t asmCount = 0;
    void** asms = g_i2.domain_get_assemblies(g_domain, &asmCount);
    for (size_t a = 0; a < asmCount; a++) {
        void* image = g_i2.assembly_get_image(asms[a]);
        if (!image) continue;
        size_t n = g_i2.image_get_class_count ? g_i2.image_get_class_count(image) : 0;
        for (size_t i = 0; i < n; i++) {
            void* klass = g_i2.image_get_class(image, i);
            if (!klass) continue;
            const char* cn = g_i2.class_get_name(klass);
            if (cn && strcmp(cn, name) == 0) {
                g_cache.classes[name] = klass;
                LogF("class %s fallback-scan => %p", name, klass);
                return klass;
            }
        }
    }
    // brute: class_from_name on every image with common namespaces
    static int diag = 0;
    bool doLog = (diag++ % 300) == 0;
    size_t asm2 = 0;
    void** asms2 = g_i2.domain_get_assemblies(g_domain, &asm2);
    for (size_t a = 0; a < asm2; a++) {
        void* image = g_i2.assembly_get_image(asms2[a]);
        if (!image) continue;
        const char* in = g_i2.image_get_name(image);
        if (doLog) LogF("scan image %s enumCount=%d", in ? in : "?", g_enumCount);
        if (!g_i2.class_from_name) continue;
        void* k = g_i2.class_from_name(image, "", name);
        if (!k) k = g_i2.class_from_name(image, "Nova.Hotfix", name);
        if (!k) k = g_i2.class_from_name(image, "UnityEngine", name);
        if (k) {
            g_cache.classes[name] = k;
            LogF("class %s brute image=%s => %p", name, in ? in : "?", k);
            return k;
        }
    }
    LogF("class %s NOT FOUND", name);
    return nullptr;
}

size_t FieldOffset(void* klass, const char* field) {
    char key[256];
    snprintf(key, sizeof(key), "%p:%s", klass, field);
    auto it = g_cache.fields.find(key);
    if (it != g_cache.fields.end()) return it->second;
    for (void* k = klass; k; k = g_i2.class_get_parent(k)) {
        void* f = g_i2.class_get_field_from_name(k, field);
        if (f) {
            size_t off = g_i2.field_get_offset(f);
            g_cache.fields[key] = off;
            return off;
        }
    }
    g_cache.fields[key] = SIZE_MAX;
    return SIZE_MAX;
}

void* Method(void* klass, const char* name, int argc) {
    char key[256];
    snprintf(key, sizeof(key), "%p:%s:%d", klass, name, argc);
    auto it = g_cache.methods.find(key);
    if (it != g_cache.methods.end()) return it->second;
    for (void* k = klass; k; k = g_i2.class_get_parent(k)) {
        void* m = g_i2.class_get_method_from_name(k, name, argc);
        if (m) {
            g_cache.methods[key] = m;
            return m;
        }
    }
    g_cache.methods[key] = nullptr;
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

bool IsInGameAssembly(void* fn) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga || !fn) return false;
    uint8_t* base = (uint8_t*)ga;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    uint8_t* p = (uint8_t*)fn;
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}
