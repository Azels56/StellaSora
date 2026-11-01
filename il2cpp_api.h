#pragma once
// Thin wrapper around the IL2CPP runtime exports the menu uses.
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <cstdarg>

void LogF(const char* fmt, ...);

using il2cpp_domain_get_t = void* (*)();
using il2cpp_thread_attach_t = void* (*)(void*);
using il2cpp_domain_get_assemblies_t = void** (*)(void*, size_t*);
using il2cpp_assembly_get_image_t = void* (*)(void*);
using il2cpp_image_get_name_t = const char* (*)(void*);
using il2cpp_image_get_class_count_t = size_t (*)(void*);
using il2cpp_image_get_class_t = void* (*)(void*, size_t);
using il2cpp_class_get_name_t = const char* (*)(void*);
using il2cpp_class_get_namespace_t = const char* (*)(void*);
using il2cpp_class_get_parent_t = void* (*)(void*);
using il2cpp_class_get_method_from_name_t = void* (*)(void*, const char*, int);
using il2cpp_class_from_name_t = void* (*)(void*, const char*, const char*);
using il2cpp_class_for_each_t = void (*)(void (*)(void*, void*), void*);
using il2cpp_runtime_invoke_t = void* (*)(void*, void*, void**, void**);
using il2cpp_class_get_field_from_name_t = void* (*)(void*, const char*);
using il2cpp_field_get_offset_t = size_t (*)(void*);
using il2cpp_class_get_type_t = void* (*)(void*);
using il2cpp_type_get_object_t = void* (*)(void*);
using il2cpp_object_get_class_t = void* (*)(void*);
using il2cpp_array_element_size_t = int (*)(void*);
using il2cpp_string_new_t = void* (*)(const char*);

struct I2 {
    il2cpp_domain_get_t domain_get = nullptr;
    il2cpp_thread_attach_t thread_attach = nullptr;
    il2cpp_domain_get_assemblies_t domain_get_assemblies = nullptr;
    il2cpp_assembly_get_image_t assembly_get_image = nullptr;
    il2cpp_image_get_name_t image_get_name = nullptr;
    il2cpp_image_get_class_count_t image_get_class_count = nullptr;
    il2cpp_image_get_class_t image_get_class = nullptr;
    il2cpp_class_get_name_t class_get_name = nullptr;
    il2cpp_class_get_namespace_t class_get_namespace = nullptr;
    il2cpp_class_get_parent_t class_get_parent = nullptr;
    il2cpp_class_get_method_from_name_t class_get_method_from_name = nullptr;
    il2cpp_class_from_name_t class_from_name = nullptr;
    il2cpp_class_for_each_t class_for_each = nullptr;
    il2cpp_runtime_invoke_t runtime_invoke = nullptr;
    il2cpp_class_get_field_from_name_t class_get_field_from_name = nullptr;
    il2cpp_field_get_offset_t field_get_offset = nullptr;
    il2cpp_class_get_type_t class_get_type = nullptr;
    il2cpp_type_get_object_t type_get_object = nullptr;
    il2cpp_object_get_class_t object_get_class = nullptr;
    il2cpp_array_element_size_t array_element_size = nullptr;
    il2cpp_string_new_t string_new = nullptr;
    bool ready = false;
};

extern I2 g_i2;
extern void* g_domain;
extern thread_local bool g_attached;

bool ResolveExports();
void EnsureThread();
void* ImageByName(const char* name);
void* FindClassByName(const char* name);
size_t FieldOffset(void* klass, const char* field);
void* Method(void* klass, const char* name, int argc);
bool Invoke(void* klass, const char* name, int argc, void* obj, void** params, int64_t* outInt = nullptr, bool* outBool = nullptr);
bool Invoke8(void* klass, const char* name, void* obj, void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7);
void* InvokeStaticList(void* klass, const char* name, int argc, void** params);
int ArrayLength(void* arr);
void* FieldPtr(void* obj, const char* field);
int ListSize(void* list);
void* ListItem(void* list, int i);
int64_t ListLong(void* list, int i);
bool IsInGameAssembly(void* fn);
