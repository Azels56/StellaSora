// Anti-cheat stealth layer — see Stealth.h.
//
// Reconstructed loader behaviour, ported to this payload:
//   ScrubDebugState  PEB + heap debug flags
//   HideAllThreads   ThreadHideFromDebugger across the process
//   UnlinkSelf       PEB loader-list unlink + SizeOfHeaders wipe
//   Bootstrap        hidden worker: scrub -> unlink -> payload threads -> watcher
//   HookLoadLibraryExW  LdrRegisterDllNotification subscription (no code patch)

#include "Stealth.h"

#include <winternl.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

#include "overlay.h"
#include "lua_bridge.h"
#include "features.h"
#include "il2cpp_api.h"

// ---------------------------------------------------------------------------
// Layout block. Every offset the layer walks lives here and nowhere else.
// ---------------------------------------------------------------------------
//   PEB   +0x02 BeingDebugged   +0x18 Ldr   +0x20 ProcessParameters
//         +0x30 ProcessHeap    +0xBC NtGlobalFlag
//   LDR   +0x10 InLoadOrderModuleList
//         +0x20 InMemoryOrderModuleList
//         +0x30 InInitializationOrderModuleList
//   ENT   +0x00/+0x10/+0x20 the three links, +0x30 DllBase, +0x40 SizeOfImage
//         +0x48 FullDllName, +0x58 BaseDllName
//   HEAP  +0x70 Flags         +0x74 ForceFlags
struct UniString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct LdrEntry
{
    LIST_ENTRY InLoadOrderLinks;            // +0x00
    LIST_ENTRY InMemoryOrderLinks;          // +0x10
    LIST_ENTRY InInitializationOrderLinks;  // +0x20
    PVOID      DllBase;                     // +0x30
    PVOID      EntryPoint;                  // +0x38
    ULONG      SizeOfImage;                 // +0x40
    UniString  FullDllName;                 // +0x48
    UniString  BaseDllName;                 // +0x58
};

struct LdrData
{
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;          // +0x10
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct Peb
{
    BYTE     InheritedAddressSpace;   // +0x00
    BYTE     ReadImageFileExecOptions;// +0x01
    BYTE     BeingDebugged;           // +0x02
    BYTE     BitField;                // +0x03
    DWORD    Pad0;                    // +0x04
    PVOID    Mutant;                  // +0x08
    PVOID    ImageBaseAddress;        // +0x10
    LdrData* Ldr;                     // +0x18
    PVOID    ProcessParameters;       // +0x20
    PVOID    SubSystemData;           // +0x28
    PVOID    ProcessHeap;             // +0x30
    BYTE     Rest[0xBC - 0x38];
    ULONG    NtGlobalFlag;            // +0xBC
};

static_assert(offsetof(Peb, Ldr) == 0x18, "PEB->Ldr");
static_assert(offsetof(Peb, ProcessHeap) == 0x30, "PEB->ProcessHeap");
static_assert(offsetof(Peb, NtGlobalFlag) == 0xBC, "PEB->NtGlobalFlag");
static_assert(offsetof(LdrEntry, DllBase) == 0x30, "LdrEntry->DllBase");
static_assert(offsetof(LdrEntry, BaseDllName) == 0x58, "LdrEntry->BaseDllName");
static_assert(offsetof(LdrData, InLoadOrderModuleList) == 0x10, "LdrData->InLoadOrder");

namespace stealth {

// Resolves the 49 UMPDC exports into the proxy table (defined at the bottom).
void LoadProxyImports();

namespace {

// ---------------------------------------------------------------------------
// String discipline. Nothing the layer resolves by name exists in .rdata as
// plaintext: module and export names are FNV-1a hashes resolved at compile
// time, and the proxy name table is stored XOR-masked and decrypted onto the
// stack one entry at a time.
// ---------------------------------------------------------------------------
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime  = 0x100000001b3ull;
constexpr char     kMask      = 0x5A;

constexpr uint64_t HashA(const char* s)
{
    uint64_t h = kFnvOffset;
    while (*s)
    {
        h ^= static_cast<unsigned char>(*s++);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t HashW(const wchar_t* s, size_t count)
{
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < count; ++i)
    {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c + 32);
        if (c > 0x7F)
            continue;
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime;
    }
    return h;
}

template <size_t N>
struct Enc
{
    char data[N];
    constexpr Enc(const char* s) : data{}
    {
        for (size_t i = 0; i + 1 < N && s[i]; ++i)
            data[i] = static_cast<char>(s[i] ^ kMask);
    }
};

#define PX(s) Enc<48>(s)

void Unmask(const Enc<48>& e, char (&out)[48])
{
    for (size_t i = 0; i < 48; ++i)
        out[i] = static_cast<char>(e.data[i] ^ kMask);
    out[47] = 0;
}

// ---------------------------------------------------------------------------
// ntdll surface, resolved by hash.
// ---------------------------------------------------------------------------
using NtCreateThreadEx_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE,
                                            PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
using NtDelayExecution_t = NTSTATUS(NTAPI*)(BOOLEAN, PLARGE_INTEGER);
using NtSetInformationThread_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
using NtGetNextThread_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
using LdrLoadDll_t = NTSTATUS(NTAPI*)(PWSTR, PULONG, PUNICODE_STRING, PHANDLE);
using LdrUnloadDll_t = NTSTATUS(NTAPI*)(PVOID);

NtCreateThreadEx_t        pNtCreateThreadEx        = nullptr;
NtDelayExecution_t        pNtDelayExecution        = nullptr;
NtSetInformationThread_t  pNtSetInformationThread  = nullptr;
NtGetNextThread_t         pNtGetNextThread         = nullptr;
LdrLoadDll_t              pLdrLoadDll              = nullptr;
LdrUnloadDll_t            pLdrUnloadDll            = nullptr;

// ThreadHideFromDebugger. Hides the thread from every debug event path.
constexpr ULONG kThreadHideFromDebugger = 17;
// NtCreateThreadEx takes THREAD_CREATE_FLAGS_* bits. HIDE_FROM_DEBUGGER is 4;
// the 0x20000000 the original loader passed is not in THREAD_CREATE_FLAGS_ALL
// and the call fails with STATUS_INVALID_PARAMETER on current builds.
constexpr ULONG kThreadCreateHideFromDebugger = 0x00000004;

constexpr bool NtOk(NTSTATUS s)
{
    return static_cast<LONG>(s) >= 0;
}

// ---------------------------------------------------------------------------
// Module / export resolution without a single named lookup.
// ---------------------------------------------------------------------------
Peb* CurrentPeb()
{
    return reinterpret_cast<Peb*>(__readgsqword(0x60));
}

HMODULE ModuleByHash(uint64_t nameHash)
{
    Peb* peb = CurrentPeb();
    if (!peb || !peb->Ldr)
        return nullptr;

    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* link = head->Flink; link && link != head; link = link->Flink)
    {
        auto* entry = CONTAINING_RECORD(link, LdrEntry, InMemoryOrderLinks);
        if (!entry->BaseDllName.Buffer || entry->BaseDllName.Length == 0)
            continue;
        if (HashW(entry->BaseDllName.Buffer, entry->BaseDllName.Length / sizeof(wchar_t)) == nameHash)
            return static_cast<HMODULE>(entry->DllBase);
    }
    return nullptr;
}

void* ExportByHash(HMODULE module, uint64_t nameHash, int depth = 0)
{
    if (!module || depth > 8)
        return nullptr;

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0)
        return nullptr;

    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* ordinals = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i)
    {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (HashA(name) != nameHash)
            continue;

        const DWORD rva = functions[ordinals[i]];
        if (rva == 0)
            return nullptr;

        // Forwarder: the RVA lands inside the export directory and the target
        // reads "OTHERDLL.RealName".
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
        {
            const char* fwd = reinterpret_cast<const char*>(base + rva);
            const char* dot = nullptr;
            for (const char* p = fwd; *p; ++p)
                if (*p == '.')
                    dot = p;
            if (!dot)
                return nullptr;

            char dllName[64] = {};
            const size_t dllLen = static_cast<size_t>(dot - fwd);
            if (dllLen >= sizeof(dllName))
                return nullptr;
            for (size_t j = 0; j < dllLen; ++j)
            {
                char c = fwd[j];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + 32);
                dllName[j] = c;
            }

            HMODULE target = ModuleByHash(HashA(dllName));
            if (!target)
                return nullptr;

            return ExportByHash(target, HashA(dot + 1), depth + 1);
        }

        return base + rva;
    }
    return nullptr;
}

bool ResolveNtdll()
{
    HMODULE ntdll = ModuleByHash(HashA("ntdll.dll"));
    if (!ntdll)
        return false;

    pNtCreateThreadEx = reinterpret_cast<NtCreateThreadEx_t>(
        ExportByHash(ntdll, HashA("NtCreateThreadEx")));
    pNtDelayExecution = reinterpret_cast<NtDelayExecution_t>(
        ExportByHash(ntdll, HashA("NtDelayExecution")));
    pNtSetInformationThread = reinterpret_cast<NtSetInformationThread_t>(
        ExportByHash(ntdll, HashA("NtSetInformationThread")));
    pNtGetNextThread = reinterpret_cast<NtGetNextThread_t>(
        ExportByHash(ntdll, HashA("NtGetNextThread")));
    pLdrLoadDll = reinterpret_cast<LdrLoadDll_t>(
        ExportByHash(ntdll, HashA("LdrLoadDll")));
    pLdrUnloadDll = reinterpret_cast<LdrUnloadDll_t>(
        ExportByHash(ntdll, HashA("LdrUnloadDll")));

    return pNtCreateThreadEx && pNtDelayExecution;
}

void Delay(unsigned milliseconds)
{
    if (pNtDelayExecution)
    {
        // Negative LARGE_INTEGER = relative, 100ns units.
        LARGE_INTEGER interval;
        interval.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
        pNtDelayExecution(FALSE, &interval);
        return;
    }
    Sleep(milliseconds);
}

// ---------------------------------------------------------------------------
// Proxy table: 49 UMPDC exports. Names are stored masked and decrypted into a
// stack buffer at resolve time, so a string scan of the module finds nothing
// that reads as a power-management API.
// ---------------------------------------------------------------------------
void* g_proxy[49] = {};
HMODULE g_proxyModule = nullptr;

const Enc<48> kProxyNames[49] = {
    PX("PdcAcquireRwLockExclusive"),
    PX("PdcActivationClientActivityRequest"),
    PX("PdcActivationClientRegister"),
    PX("PdcActivationClientUnregister"),
    PX("PdcAllocate"),
    PX("PdcFree"),
    PX("PdcNotificationClientAcknowledge"),
    PX("PdcNotificationClientRegister"),
    PX("PdcNotificationClientUnregister"),
    PX("PdcPortClose"),
    PX("PdcPortOpen"),
    PX("PdcPortSendMessage"),
    PX("PdcPortSendMessageSynchronously"),
    PX("PdcPpmProfileClientRegister"),
    PX("PdcPpmProfileClientUnregister"),
    PX("PdcPpmProfileDisable"),
    PX("PdcPpmProfileEnable"),
    PX("PdcReleaseRwLockExclusive"),
    PX("PdcResiliencyClientAcknowledge"),
    PX("PdcResiliencyClientRegister"),
    PX("PdcResiliencyClientUnregister"),
    PX("PdcRwLockInitialize"),
    PX("PdcSignalClientPulse"),
    PX("PdcSignalClientRegister"),
    PX("PdcSignalClientSetActive"),
    PX("PdcSignalClientUnregister"),
    PX("PdcSleep"),
    PX("PdcTaskClientRegister"),
    PX("PdcTaskClientRequest"),
    PX("PdcTaskClientUnregister"),
    PX("Pdcv2ActivationClientActivate"),
    PX("Pdcv2ActivationClientDeactivate"),
    PX("Pdcv2ActivationClientRegister"),
    PX("Pdcv2ActivationClientRenewActivation"),
    PX("Pdcv2ActivationClientSetBrokeredProcessId"),
    PX("Pdcv2ActivationClientUnregister"),
    PX("SleepstudyHelperBlockerActiveDereference"),
    PX("SleepstudyHelperBlockerActiveReference"),
    PX("SleepstudyHelperBuildBlocker"),
    PX("SleepstudyHelperCreateBlockerFromGuid"),
    PX("SleepstudyHelperCreateLibrary"),
    PX("SleepstudyHelperCreateLibraryEx"),
    PX("SleepstudyHelperDestroyBlocker"),
    PX("SleepstudyHelperDestroyBlockerBuilder"),
    PX("SleepstudyHelperDestroyLibrary"),
    PX("SleepstudyHelperGetBlockerGuid"),
    PX("SleepstudyHelperSetBlockerFriendlyName"),
    PX("SleepstudyHelperSetBlockerParentHandle"),
    PX("SleepstudyHelperSetBlockerVisible"),
};

// ---------------------------------------------------------------------------
// Header wipe. Zeroes the whole SizeOfHeaders region - DOS stub, NT headers,
// section table, data directories - then restores protection read-only so a
// later write attempt from a scanner trips instead of succeeding silently.
// ---------------------------------------------------------------------------
void WipeHeaders(HMODULE self)
{
    auto* base = reinterpret_cast<uint8_t*>(self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    const DWORD size = nt->OptionalHeader.SizeOfHeaders;
    if (size == 0 || size > 0x1000 * 64)
        return;

    DWORD old = 0;
    if (!VirtualProtect(base, size, PAGE_READWRITE, &old))
        return;

    SecureZeroMemory(base, size);
    VirtualProtect(base, size, PAGE_READONLY, &old);
}

// ---------------------------------------------------------------------------
// Debug-state scrub. Runs before anything else touches the process: clears the
// PEB flags a check reads, clears the default heap flags a heap-walk check
// reads, and hides every thread in the process from debug events.
// ---------------------------------------------------------------------------
void ScrubDebugState()
{
    Peb* peb = CurrentPeb();
    if (!peb)
        return;

    peb->BeingDebugged = 0;
    peb->NtGlobalFlag = 0;

    if (peb->ProcessHeap)
    {
        auto* heap = reinterpret_cast<uint8_t*>(peb->ProcessHeap);
        // HEAP_GROWABLE, no tail checking, no free checking, no validate.
        *reinterpret_cast<DWORD*>(heap + 0x70) = 2;
        *reinterpret_cast<DWORD*>(heap + 0x74) = 0;
    }
}

void HideCurrentThread()
{
    if (pNtSetInformationThread)
        pNtSetInformationThread(GetCurrentThread(), kThreadHideFromDebugger, nullptr, 0);
}

void HideAllThreads()
{
    if (!pNtSetInformationThread || !pNtGetNextThread)
        return;

    HANDLE current = nullptr;
    HANDLE next = nullptr;

    while (NtOk(pNtGetNextThread(GetCurrentProcess(), current,
                                 THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION,
                                 0, 0, &next)))
    {
        if (current)
            CloseHandle(current);
        current = next;
        next = nullptr;
        pNtSetInformationThread(current, kThreadHideFromDebugger, nullptr, 0);
    }

    if (current)
        CloseHandle(current);
}

// ---------------------------------------------------------------------------
// Loader unlink. Drops the entry from all three lists, zeroes the link fields
// so a heap scan for a node pointing into the list finds a dead block, zeroes
// both name buffers, then zeroes the entry itself.
// ---------------------------------------------------------------------------
void UnlinkSelf(HMODULE self)
{
    if (!self)
        return;

    Peb* peb = CurrentPeb();
    if (!peb || !peb->Ldr)
        return;

    LIST_ENTRY* head = &peb->Ldr->InLoadOrderModuleList;
    for (LIST_ENTRY* link = head->Flink; link && link != head; link = link->Flink)
    {
        auto* entry = CONTAINING_RECORD(link, LdrEntry, InLoadOrderLinks);
        if (entry->DllBase != self)
            continue;

        if (entry->InLoadOrderLinks.Flink && entry->InLoadOrderLinks.Blink)
        {
            entry->InLoadOrderLinks.Blink->Flink = entry->InLoadOrderLinks.Flink;
            entry->InLoadOrderLinks.Flink->Blink = entry->InLoadOrderLinks.Blink;
        }

        if (entry->InMemoryOrderLinks.Flink && entry->InMemoryOrderLinks.Blink)
        {
            entry->InMemoryOrderLinks.Blink->Flink = entry->InMemoryOrderLinks.Flink;
            entry->InMemoryOrderLinks.Flink->Blink = entry->InMemoryOrderLinks.Blink;
        }

        if (entry->InInitializationOrderLinks.Flink && entry->InInitializationOrderLinks.Blink)
        {
            entry->InInitializationOrderLinks.Blink->Flink = entry->InInitializationOrderLinks.Flink;
            entry->InInitializationOrderLinks.Flink->Blink = entry->InInitializationOrderLinks.Blink;
        }

        if (entry->BaseDllName.Buffer && entry->BaseDllName.MaximumLength)
            SecureZeroMemory(entry->BaseDllName.Buffer, entry->BaseDllName.MaximumLength);
        if (entry->FullDllName.Buffer && entry->FullDllName.MaximumLength)
            SecureZeroMemory(entry->FullDllName.Buffer, entry->FullDllName.MaximumLength);

        entry->BaseDllName.Length = 0;
        entry->BaseDllName.MaximumLength = 0;
        entry->FullDllName.Length = 0;
        entry->FullDllName.MaximumLength = 0;
        entry->DllBase = nullptr;
        entry->SizeOfImage = 0;

        // Dead block. Anything that cached the pointer reads zeros, not a node
        // that still links back into the loader's list. Stops at 0x68 so the
        // entry's HashLinks stay intact and LdrpFindLoadedDll's bucket walk
        // survives a later LoadLibrary.
        SecureZeroMemory(entry, sizeof(LdrEntry));
        break;
    }

    WipeHeaders(self);
}

// ---------------------------------------------------------------------------
// Load-notification subscription. This replaces the kernelbase!LoadLibraryExW
// detour entirely: no byte in a system module is ever written, so a .text
// checksum of kernelbase stays clean, and the callback still fires on every
// module the process loads.
//
// The callback runs under the loader lock, so it only sets a flag. The worker
// thread drains the flag outside the lock - calling back into LoadLibrary from
// inside the notification would deadlock.
// ---------------------------------------------------------------------------
struct LdrDllLoadedData
{
    ULONG Flags;
    const UniString* FullDllName;
    const UniString* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};

struct LdrDllUnloadedData
{
    ULONG Flags;
    const UniString* FullDllName;
    const UniString* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};

union LdrDllNotificationData
{
    LdrDllLoadedData Loaded;
    LdrDllUnloadedData Unloaded;
};

using LdrDllNotificationFn = void(NTAPI*)(ULONG, const LdrDllNotificationData*, PVOID);
using LdrRegisterDllNotification_t = NTSTATUS(NTAPI*)(ULONG, LdrDllNotificationFn, PVOID, PVOID*);
using LdrUnregisterDllNotification_t = NTSTATUS(NTAPI*)(PVOID);

LdrUnregisterDllNotification_t pLdrUnregisterDllNotification = nullptr;
PVOID g_notificationCookie = nullptr;

volatile LONG g_rearmPending = 0;
volatile LONG g_started = 0;

void NTAPI OnDllNotification(ULONG reason, const LdrDllNotificationData*, PVOID)
{
    // LDR_DLL_NOTIFICATION_REASON_LOADED
    if (reason == 1)
        InterlockedExchange(&g_rearmPending, 1);
}

void HookLoadLibraryExW()
{
    if (g_notificationCookie)
        return;

    HMODULE ntdll = ModuleByHash(HashA("ntdll.dll"));
    if (!ntdll)
        return;

    auto reg = reinterpret_cast<LdrRegisterDllNotification_t>(
        ExportByHash(ntdll, HashA("LdrRegisterDllNotification")));
    pLdrUnregisterDllNotification = reinterpret_cast<LdrUnregisterDllNotification_t>(
        ExportByHash(ntdll, HashA("LdrUnregisterDllNotification")));

    if (!reg)
        return;

    reg(0, &OnDllNotification, nullptr, &g_notificationCookie);
}

// ---------------------------------------------------------------------------
// Payload thread creation. Same hide flag as the worker, so nothing spawned by
// the layer shows up in a debugger's thread list.
// ---------------------------------------------------------------------------
HANDLE CreateHiddenThread(LPTHREAD_START_ROUTINE start, LPVOID arg)
{
    if (pNtCreateThreadEx)
    {
        HANDLE thread = nullptr;
        NTSTATUS status = pNtCreateThreadEx(&thread, THREAD_ALL_ACCESS, nullptr,
                                            GetCurrentProcess(),
                                            reinterpret_cast<PVOID>(start), arg,
                                            kThreadCreateHideFromDebugger, 0, 0, 0, nullptr);
        if (NtOk(status) && thread)
            return thread;
    }

    HANDLE thread = CreateThread(nullptr, 0, start, arg, 0, nullptr);
    if (thread && pNtSetInformationThread)
        pNtSetInformationThread(thread, kThreadHideFromDebugger, nullptr, 0);
    return thread;
}

void SpawnPayloadThreads()
{
    HANDLE h[4] = {
        CreateHiddenThread(InitThread, nullptr),       // D3D11 hook + menu
        CreateHiddenThread(HotkeyThread, nullptr),     // INSERT / F-key poller
        CreateHiddenThread(LuaHookThread, nullptr),    // xLua hooks
        CreateHiddenThread(LuaConsoleThread, nullptr), // diag + script queue
    };
    for (HANDLE t : h)
        if (t)
            CloseHandle(t);
}

// ---------------------------------------------------------------------------
// Worker thread body.
// ---------------------------------------------------------------------------
DWORD WINAPI Bootstrap(LPVOID self)
{
    // Give the process a beat to finish its own startup. NtDelayExecution, not
    // Sleep: no kernel32 import for an anti-cheat to detour and watch.
    Delay(150);

    ScrubDebugState();
    HideCurrentThread();
    HideAllThreads();

    // The loader-list unlink is deliberately NOT done here. The process is
    // still booting and the loader is walking that same list from its own
    // threads; pulling an entry out from under it makes GetModuleFileName(NULL)
    // read a zeroed node, and the Unity player then reports "Data folder not
    // found" with a blank application folder and dies before the first frame.
    // It is armed here and fired from the watcher once rendering has started.

    SpawnPayloadThreads();
    HookLoadLibraryExW();

    // Steady-state watcher. Drains the module-load flag outside the loader
    // lock and re-arms the xLua hook if xlua.dll arrived after the first
    // attempt gave up.
    bool unlinked = false;
    ULONGLONG waited = 0;
    for (;;)
    {
        Delay(60);
        waited += 60;

        // First Present means the player is up: every module its boot needed is
        // loaded and its paths are resolved. 20 s cap so a renderer that never
        // presents (headless, or the hook missed) still gets the unlink.
        if (!unlinked && (g_overlayBooted || waited >= 20000))
        {
            UnlinkSelf(static_cast<HMODULE>(self));
            unlinked = true;
        }

        if (InterlockedExchange(&g_rearmPending, 0) != 0)
        {
            if (!g_luaHooked)
            {
                HANDLE t = CreateHiddenThread(LuaHookThread, nullptr);
                if (t)
                    CloseHandle(t);
            }
        }
    }
}

} // namespace

bool Start(HMODULE self)
{
    if (!self)
        return false;

    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return true; // already started; the watcher owns the layer from here

    if (!ResolveNtdll())
        return false;

    LoadProxyImports(); // optional cover module; failure is not fatal

    HANDLE thread = nullptr;
    NTSTATUS status = pNtCreateThreadEx(&thread, THREAD_ALL_ACCESS, nullptr,
                                        GetCurrentProcess(),
                                        reinterpret_cast<PVOID>(&Bootstrap), self,
                                        kThreadCreateHideFromDebugger, 0, 0, 0, nullptr);

    if (!NtOk(status) || !thread)
    {
        // Hidden path unavailable. Drop the proxy module so nothing sits in the
        // process with no worker, and let the caller fall back to plain threads.
        if (pLdrUnloadDll && g_proxyModule)
            pLdrUnloadDll(g_proxyModule);
        g_proxyModule = nullptr;
        InterlockedExchange(&g_started, 0);
        return false;
    }

    CloseHandle(thread);
    return true;
}

void Stop()
{
    if (pLdrUnregisterDllNotification && g_notificationCookie)
    {
        pLdrUnregisterDllNotification(g_notificationCookie);
        g_notificationCookie = nullptr;
    }

    if (g_proxyModule)
    {
        if (pLdrUnloadDll)
            pLdrUnloadDll(g_proxyModule);
        else
            FreeLibrary(g_proxyModule);
        g_proxyModule = nullptr;
    }
}

void LoadProxyImports()
{
    if (g_proxyModule)
        return;

    // Already loaded by the process? Use it. Only pull it in if it is genuinely
    // absent, and pull it through LdrLoadDll so no kernelbase import is
    // involved.
    HMODULE module = ModuleByHash(HashA("umpdc.dll"));

    if (!module && pLdrLoadDll)
    {
        WCHAR path[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 16u)
            return;

        static const WCHAR kFile[] = L"\\UMPDC.dll";
        UINT i = 0;
        for (; kFile[i] && static_cast<UINT>(len + i) + 1u < MAX_PATH; ++i)
            path[len + i] = kFile[i];
        path[len + i] = 0;

        UniString name;
        name.Buffer = path;
        name.Length = static_cast<USHORT>((len + i) * sizeof(WCHAR));
        name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(WCHAR));

        HANDLE handle = nullptr;
        if (!NtOk(pLdrLoadDll(nullptr, nullptr, reinterpret_cast<PUNICODE_STRING>(&name), &handle)))
            return;

        module = static_cast<HMODULE>(handle);
    }

    if (!module)
        return;

    g_proxyModule = module;

    for (int i = 0; i < 49; ++i)
    {
        char plain[48];
        Unmask(kProxyNames[i], plain);
        g_proxy[i] = ExportByHash(module, HashA(plain));
        SecureZeroMemory(plain, sizeof(plain));
    }
}

// Proxy thunks. Declared here, defined below; only the first has a call site.
void PdcAcquireRwLockExclusive();
void PdcActivationClientActivityRequest();
void PdcActivationClientRegister();
void PdcActivationClientUnregister();
void PdcAllocate();
void PdcFree();
void PdcNotificationClientAcknowledge();
void PdcNotificationClientRegister();
void PdcNotificationClientUnregister();
void PdcPortClose();
void PdcPortOpen();
void PdcPortSendMessage();
void PdcPortSendMessageSynchronously();
void PdcPpmProfileClientRegister();
void PdcPpmProfileClientUnregister();
void PdcPpmProfileDisable();
void PdcPpmProfileEnable();
void PdcReleaseRwLockExclusive();
void PdcResiliencyClientAcknowledge();
void PdcResiliencyClientRegister();
void PdcResiliencyClientUnregister();
void PdcRwLockInitialize();
void PdcSignalClientPulse();
void PdcSignalClientRegister();
void PdcSignalClientSetActive();
void PdcSignalClientUnregister();
void PdcSleep();
void PdcTaskClientRegister();
void PdcTaskClientRequest();
void PdcTaskClientUnregister();
void Pdcv2ActivationClientActivate();
void Pdcv2ActivationClientDeactivate();
void Pdcv2ActivationClientRegister();
void Pdcv2ActivationClientRenewActivation();
void Pdcv2ActivationClientSetBrokeredProcessId();
void Pdcv2ActivationClientUnregister();
void SleepstudyHelperBlockerActiveDereference();
void SleepstudyHelperBlockerActiveReference();
void SleepstudyHelperBuildBlocker();
void SleepstudyHelperCreateBlockerFromGuid();
void SleepstudyHelperCreateLibrary();
void SleepstudyHelperCreateLibraryEx();
void SleepstudyHelperDestroyBlocker();
void SleepstudyHelperDestroyBlockerBuilder();
void SleepstudyHelperDestroyLibrary();
void SleepstudyHelperGetBlockerGuid();
void SleepstudyHelperSetBlockerFriendlyName();
void SleepstudyHelperSetBlockerParentHandle();
void SleepstudyHelperSetBlockerVisible();

// Pins the thunks so /OPT:REF cannot drop them - the original keeps all 49.
const void* volatile g_thunkTable[49] = {
    reinterpret_cast<void*>(&PdcAcquireRwLockExclusive),
    reinterpret_cast<void*>(&PdcActivationClientActivityRequest),
    reinterpret_cast<void*>(&PdcActivationClientRegister),
    reinterpret_cast<void*>(&PdcActivationClientUnregister),
    reinterpret_cast<void*>(&PdcAllocate),
    reinterpret_cast<void*>(&PdcFree),
    reinterpret_cast<void*>(&PdcNotificationClientAcknowledge),
    reinterpret_cast<void*>(&PdcNotificationClientRegister),
    reinterpret_cast<void*>(&PdcNotificationClientUnregister),
    reinterpret_cast<void*>(&PdcPortClose),
    reinterpret_cast<void*>(&PdcPortOpen),
    reinterpret_cast<void*>(&PdcPortSendMessage),
    reinterpret_cast<void*>(&PdcPortSendMessageSynchronously),
    reinterpret_cast<void*>(&PdcPpmProfileClientRegister),
    reinterpret_cast<void*>(&PdcPpmProfileClientUnregister),
    reinterpret_cast<void*>(&PdcPpmProfileDisable),
    reinterpret_cast<void*>(&PdcPpmProfileEnable),
    reinterpret_cast<void*>(&PdcReleaseRwLockExclusive),
    reinterpret_cast<void*>(&PdcResiliencyClientAcknowledge),
    reinterpret_cast<void*>(&PdcResiliencyClientRegister),
    reinterpret_cast<void*>(&PdcResiliencyClientUnregister),
    reinterpret_cast<void*>(&PdcRwLockInitialize),
    reinterpret_cast<void*>(&PdcSignalClientPulse),
    reinterpret_cast<void*>(&PdcSignalClientRegister),
    reinterpret_cast<void*>(&PdcSignalClientSetActive),
    reinterpret_cast<void*>(&PdcSignalClientUnregister),
    reinterpret_cast<void*>(&PdcSleep),
    reinterpret_cast<void*>(&PdcTaskClientRegister),
    reinterpret_cast<void*>(&PdcTaskClientRequest),
    reinterpret_cast<void*>(&PdcTaskClientUnregister),
    reinterpret_cast<void*>(&Pdcv2ActivationClientActivate),
    reinterpret_cast<void*>(&Pdcv2ActivationClientDeactivate),
    reinterpret_cast<void*>(&Pdcv2ActivationClientRegister),
    reinterpret_cast<void*>(&Pdcv2ActivationClientRenewActivation),
    reinterpret_cast<void*>(&Pdcv2ActivationClientSetBrokeredProcessId),
    reinterpret_cast<void*>(&Pdcv2ActivationClientUnregister),
    reinterpret_cast<void*>(&SleepstudyHelperBlockerActiveDereference),
    reinterpret_cast<void*>(&SleepstudyHelperBlockerActiveReference),
    reinterpret_cast<void*>(&SleepstudyHelperBuildBlocker),
    reinterpret_cast<void*>(&SleepstudyHelperCreateBlockerFromGuid),
    reinterpret_cast<void*>(&SleepstudyHelperCreateLibrary),
    reinterpret_cast<void*>(&SleepstudyHelperCreateLibraryEx),
    reinterpret_cast<void*>(&SleepstudyHelperDestroyBlocker),
    reinterpret_cast<void*>(&SleepstudyHelperDestroyBlockerBuilder),
    reinterpret_cast<void*>(&SleepstudyHelperDestroyLibrary),
    reinterpret_cast<void*>(&SleepstudyHelperGetBlockerGuid),
    reinterpret_cast<void*>(&SleepstudyHelperSetBlockerFriendlyName),
    reinterpret_cast<void*>(&SleepstudyHelperSetBlockerParentHandle),
    reinterpret_cast<void*>(&SleepstudyHelperSetBlockerVisible),
};

// All 49 proxy thunks. Only the first has a call site; the rest give the module
// a plausible power-management API footprint.
void PdcAcquireRwLockExclusive()          { if (g_proxy[0])  reinterpret_cast<void (*)()>(g_proxy[0])(); }
void PdcActivationClientActivityRequest() { if (g_proxy[1])  reinterpret_cast<void (*)()>(g_proxy[1])(); }
void PdcActivationClientRegister()        { if (g_proxy[2])  reinterpret_cast<void (*)()>(g_proxy[2])(); }
void PdcActivationClientUnregister()      { if (g_proxy[3])  reinterpret_cast<void (*)()>(g_proxy[3])(); }
void PdcAllocate()                        { if (g_proxy[4])  reinterpret_cast<void (*)()>(g_proxy[4])(); }
void PdcFree()                            { if (g_proxy[5])  reinterpret_cast<void (*)()>(g_proxy[5])(); }
void PdcNotificationClientAcknowledge()   { if (g_proxy[6])  reinterpret_cast<void (*)()>(g_proxy[6])(); }
void PdcNotificationClientRegister()      { if (g_proxy[7])  reinterpret_cast<void (*)()>(g_proxy[7])(); }
void PdcNotificationClientUnregister()    { if (g_proxy[8])  reinterpret_cast<void (*)()>(g_proxy[8])(); }
void PdcPortClose()                       { if (g_proxy[9])  reinterpret_cast<void (*)()>(g_proxy[9])(); }
void PdcPortOpen()                        { if (g_proxy[10]) reinterpret_cast<void (*)()>(g_proxy[10])(); }
void PdcPortSendMessage()                 { if (g_proxy[11]) reinterpret_cast<void (*)()>(g_proxy[11])(); }
void PdcPortSendMessageSynchronously()    { if (g_proxy[12]) reinterpret_cast<void (*)()>(g_proxy[12])(); }
void PdcPpmProfileClientRegister()        { if (g_proxy[13]) reinterpret_cast<void (*)()>(g_proxy[13])(); }
void PdcPpmProfileClientUnregister()      { if (g_proxy[14]) reinterpret_cast<void (*)()>(g_proxy[14])(); }
void PdcPpmProfileDisable()               { if (g_proxy[15]) reinterpret_cast<void (*)()>(g_proxy[15])(); }
void PdcPpmProfileEnable()                { if (g_proxy[16]) reinterpret_cast<void (*)()>(g_proxy[16])(); }
void PdcReleaseRwLockExclusive()          { if (g_proxy[17]) reinterpret_cast<void (*)()>(g_proxy[17])(); }
void PdcResiliencyClientAcknowledge()     { if (g_proxy[18]) reinterpret_cast<void (*)()>(g_proxy[18])(); }
void PdcResiliencyClientRegister()        { if (g_proxy[19]) reinterpret_cast<void (*)()>(g_proxy[19])(); }
void PdcResiliencyClientUnregister()      { if (g_proxy[20]) reinterpret_cast<void (*)()>(g_proxy[20])(); }
void PdcRwLockInitialize()                { if (g_proxy[21]) reinterpret_cast<void (*)()>(g_proxy[21])(); }
void PdcSignalClientPulse()               { if (g_proxy[22]) reinterpret_cast<void (*)()>(g_proxy[22])(); }
void PdcSignalClientRegister()            { if (g_proxy[23]) reinterpret_cast<void (*)()>(g_proxy[23])(); }
void PdcSignalClientSetActive()           { if (g_proxy[24]) reinterpret_cast<void (*)()>(g_proxy[24])(); }
void PdcSignalClientUnregister()          { if (g_proxy[25]) reinterpret_cast<void (*)()>(g_proxy[25])(); }
void PdcSleep()                           { if (g_proxy[26]) reinterpret_cast<void (*)()>(g_proxy[26])(); }
void PdcTaskClientRegister()              { if (g_proxy[27]) reinterpret_cast<void (*)()>(g_proxy[27])(); }
void PdcTaskClientRequest()               { if (g_proxy[28]) reinterpret_cast<void (*)()>(g_proxy[28])(); }
void PdcTaskClientUnregister()            { if (g_proxy[29]) reinterpret_cast<void (*)()>(g_proxy[29])(); }
void Pdcv2ActivationClientActivate()      { if (g_proxy[30]) reinterpret_cast<void (*)()>(g_proxy[30])(); }
void Pdcv2ActivationClientDeactivate()    { if (g_proxy[31]) reinterpret_cast<void (*)()>(g_proxy[31])(); }
void Pdcv2ActivationClientRegister()      { if (g_proxy[32]) reinterpret_cast<void (*)()>(g_proxy[32])(); }
void Pdcv2ActivationClientRenewActivation(){ if (g_proxy[33]) reinterpret_cast<void (*)()>(g_proxy[33])(); }
void Pdcv2ActivationClientSetBrokeredProcessId() { if (g_proxy[34]) reinterpret_cast<void (*)()>(g_proxy[34])(); }
void Pdcv2ActivationClientUnregister()    { if (g_proxy[35]) reinterpret_cast<void (*)()>(g_proxy[35])(); }
void SleepstudyHelperBlockerActiveDereference() { if (g_proxy[36]) reinterpret_cast<void (*)()>(g_proxy[36])(); }
void SleepstudyHelperBlockerActiveReference()   { if (g_proxy[37]) reinterpret_cast<void (*)()>(g_proxy[37])(); }
void SleepstudyHelperBuildBlocker()       { if (g_proxy[38]) reinterpret_cast<void (*)()>(g_proxy[38])(); }
void SleepstudyHelperCreateBlockerFromGuid()    { if (g_proxy[39]) reinterpret_cast<void (*)()>(g_proxy[39])(); }
void SleepstudyHelperCreateLibrary()      { if (g_proxy[40]) reinterpret_cast<void (*)()>(g_proxy[40])(); }
void SleepstudyHelperCreateLibraryEx()    { if (g_proxy[41]) reinterpret_cast<void (*)()>(g_proxy[41])(); }
void SleepstudyHelperDestroyBlocker()     { if (g_proxy[42]) reinterpret_cast<void (*)()>(g_proxy[42])(); }
void SleepstudyHelperDestroyBlockerBuilder()    { if (g_proxy[43]) reinterpret_cast<void (*)()>(g_proxy[43])(); }
void SleepstudyHelperDestroyLibrary()     { if (g_proxy[44]) reinterpret_cast<void (*)()>(g_proxy[44])(); }
void SleepstudyHelperGetBlockerGuid()     { if (g_proxy[45]) reinterpret_cast<void (*)()>(g_proxy[45])(); }
void SleepstudyHelperSetBlockerFriendlyName()   { if (g_proxy[46]) reinterpret_cast<void (*)()>(g_proxy[46])(); }
void SleepstudyHelperSetBlockerParentHandle()   { if (g_proxy[47]) reinterpret_cast<void (*)()>(g_proxy[47])(); }
void SleepstudyHelperSetBlockerVisible()  { if (g_proxy[48]) reinterpret_cast<void (*)()>(g_proxy[48])(); }

} // namespace stealth
