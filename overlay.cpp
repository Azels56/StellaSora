// D3D11 present hook, theme, wallpaper and the ImGui menu.
#include "overlay.h"
#include "features.h"
#include "lua_bridge.h"
#include "il2cpp_api.h"
#include "str_obf.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include "imgui.h"
#include "Menu.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "MinHook.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static void DrawFloaters(ImDrawList* dl, const ImVec2& display);


// -------------------------------------------------------------- D3D11 + menu
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static IDXGISwapChain* g_rtvSwap = nullptr;
static UINT g_rtvW = 0, g_rtvH = 0;
static bool g_imguiReady = false;
bool g_menuOpen = false;  // INSERT opens it; nothing is hooked until then
volatile LONG g_overlayBooted = 0;
ImFont* g_fontBold = nullptr;
float g_wallAspect = 0.0f;
using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
static Present_t g_origPresent = nullptr;
using ResizeBuffers_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffers_t g_origResizeBuffers = nullptr;
static int g_frame = 0;
static std::string g_status = "attaching...";

// The Present and ResizeBuffers detours are .text patches in d3d11/DXGI, and
// the window subclass is an edit to the game's own window object. Both used to
// be created and torn down around each menu session, which meant patching live
// code while the game thread could be executing inside the detour — the crash
// on close. They are now installed once at startup and left installed; the
// overlay stays inert by returning straight through when nothing is drawn.
static void* g_presentAddr = nullptr;
static void* g_resizeAddr = nullptr;
static HWND g_subclassHwnd = nullptr;

static bool FloatersAlive();


static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // The subclass now lives for the whole session, so everything inside is
    // gated on the menu being on screen. An ungated handler would run for
    // every game input and swallow clicks that belong to the game.
    if (g_menuOpen && g_imguiReady) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        // Hotkeys are polled off GetAsyncKeyState instead of read here: this
        // only runs while the menu is open, and INSERT has to work when it is
        // closed. Doing both would toggle twice on a single press.
        //
        // Mouse and keyboard are gated on their own capture flags. Sharing the
        // mouse flag for both meant typing in the Luau box also drove the
        // character, and hovering the menu swallowed WASD even though ImGui
        // never wanted the keyboard.
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
            if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return 1;
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard) return 1;
            break;
        }
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}


static HWND FindGameWindow() {
    struct Ctx { DWORD pid; HWND hwnd; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
        char cls[64] = {};
        GetClassNameA(h, cls, sizeof(cls));
        if (strstr(cls, "Unity")) { c->hwnd = h; return FALSE; }
        RECT r{};
        GetWindowRect(h, &r);
        if ((r.right - r.left) > 640) { c->hwnd = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.hwnd;
}

static void CreateRTV(IDXGISwapChain* sc) {
    if (!g_dev) return;
    if (g_rtv && g_rtvSwap == sc) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    g_rtvSwap = sc;
    back->Release();
}


static void ApplyTheme() {
    menu::ApplyTheme();
}

static ID3D11ShaderResourceView* g_wallTex = nullptr;
static bool g_wallTried = false;

static bool LoadWallpaper(ID3D11Device* dev) {
    if (g_wallTex || g_wallTried || !dev) return g_wallTex != nullptr;
    g_wallTried = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) return false;
    IWICBitmapDecoder* dec = nullptr;
    const wchar_t* path = L"C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\Phoenix.jpg";
    bool ok = false;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
        IWICBitmapFrameDecode* frame = nullptr;
        if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv) {
                if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    UINT w = 0, h = 0;
                    conv->GetSize(&w, &h);
                    if (w && h) {
                        g_wallAspect = (float)w / (float)h;
                        std::vector<uint8_t> px((size_t)w * h * 4);
                        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
                            D3D11_TEXTURE2D_DESC td{};
                            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
                            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                            D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
                            ID3D11Texture2D* tex = nullptr;
                            if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex)) && tex) {
                                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                                sv.Format = td.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
                                ok = SUCCEEDED(dev->CreateShaderResourceView(tex, &sv, &g_wallTex));
                                tex->Release();
                            }
                        }
                    }
                }
                conv->Release();
            }
            frame->Release();
        }
        dec->Release();
    }
    factory->Release();
    if (ok) LogF("wallpaper loaded");
    return ok;
}

static void DrawMenu() {
    menu::Draw((ImTextureID)(intptr_t)g_wallTex, g_wallAlpha, g_dimAlpha);
    static ULONGLONG lastSave = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSave >= 2000) { lastSave = now; SaveConfig(); }
}

static void RenderOverlay(IDXGISwapChain* sc) {
    __try {
        if (!g_ctx) return;
        if (g_dev && !g_wallTex) LoadWallpaper(g_dev);
        CreateRTV(sc);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // Assigning FontScaleMain invalidates the font atlas, so only write it
        // when the Settings slider actually moved.
        if (ImGui::GetStyle().FontScaleMain != g_fontScale)
            ImGui::GetStyle().FontScaleMain = g_fontScale;
        ImGui::NewFrame();
        if (g_menuOpen) DrawMenu();
        DrawFloaters(ImGui::GetForegroundDrawList(), ImGui::GetIO().DisplaySize);
        ImGui::Render();
        if (g_rtv) {
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        g_frame++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_rtvSwap = nullptr;
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
}


static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_rtvSwap = nullptr;
    if (g_imguiReady) ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResizeBuffers(sc, count, width, height, format, flags);
    if (g_imguiReady && g_ctx) ImGui_ImplDX11_CreateDeviceObjects();
    return hr;
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    // First frame seen: the player has a swapchain, so its own startup path
    // resolution is done and the loader list is stable.
    InterlockedExchange(&g_overlayBooted, 1);

    // Nothing on screen: skip the device query, the ImGui frame and the whole
    // draw path. This is where the overlay spends almost all of its time, and
    // it now costs two reads instead of a GetDevice round-trip. menu::Animating
    // keeps the frame alive while the close fade is still running - skipping it
    // froze the animation at full alpha and made the menu pop.
    if (!g_menuOpen && !menu::Animating() && !FloatersAlive())
        return g_origPresent(sc, sync, flags);

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return g_origPresent(sc, sync, flags);

    // Unity recreates the swapchain/device when entering or leaving a match.
    // Never render with a stale device, context or back-buffer view.
    if (g_imguiReady && (dev != g_dev || sc != g_swap)) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_rtvSwap = nullptr;
        // The wallpaper is a device resource. It dies with the old device, and
        // g_wallTried would otherwise block the reload for the rest of the
        // session, leaving the menu drawing a texture that no longer exists.
        if (g_wallTex) { g_wallTex->Release(); g_wallTex = nullptr; }
        g_wallTried = false;
        ImGui_ImplDX11_Shutdown();
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
        g_dev = dev;
        dev = nullptr;
        g_swap = sc;
        g_dev->GetImmediateContext(&g_ctx);
        g_imguiReady = g_ctx && ImGui_ImplDX11_Init(g_dev, g_ctx);
    }

    if (!g_imguiReady) {
        // A previous attempt can leave a device and context behind when the
        // window was not found yet. Overwriting them without releasing pinned a
        // device reference per frame, which blocks the swapchain from ever being
        // destroyed.
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
        g_dev = dev;
        dev = nullptr;
        if (g_dev) {
            g_dev->GetImmediateContext(&g_ctx);
            g_swap = sc;
            g_hwnd = FindGameWindow();
            if (g_hwnd && g_ctx) {
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
                    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f);
                    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES)
                        g_fontBold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 28.0f);
                } else {
                    ImFontConfig fc;
                    fc.SizePixels = 20.0f;
                    io.Fonts->AddFontDefault(&fc);
                }
                ImGui::StyleColorsDark();
                ApplyTheme();
                ImGui_ImplWin32_Init(g_hwnd);
                // Only trust the backend once it actually initialised: marking
                // ready on a failed init sends NewFrame into a dead renderer.
                g_imguiReady = ImGui_ImplDX11_Init(g_dev, g_ctx);
            }
        }
    }
    if (dev) dev->Release();

    // The subclass is installed once and then left alone. Dropping it on close
    // and reinstalling on open raced the game thread's own window messages: a
    // detach landing between SetWindowLongPtrA and the bookkeeping recorded our
    // own proc as "the original", and the next subclass made WndProc call
    // itself until the stack ran out. Re-check the live proc instead of
    // trusting the cached handle, which also heals a window Unity rebuilt.
    if (g_menuOpen && g_imguiReady) {
        if (!g_hwnd || !IsWindow(g_hwnd)) {
            g_hwnd = FindGameWindow();
            g_subclassHwnd = nullptr;
            g_origWndProc = nullptr;
        }
        if (g_hwnd) {
            WNDPROC cur = (WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
            if (cur != WndProc) {
                g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
                g_subclassHwnd = g_hwnd;
            }
        }
    }

    if (g_imguiReady && g_ctx) {
        RenderOverlay(sc);
    }
    return g_origPresent(sc, sync, flags);
}


typedef int(__cdecl* luaL_loadbufferx_t)(void*, const char*, size_t, const char*, const char*);

struct DmgFloater {
    float x, y;
    ULONGLONG until;
    char text[40];
};
static DmgFloater g_floaters[64];
static CRITICAL_SECTION g_fltLock;
static INIT_ONCE g_fltOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK FloaterOnce(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_fltLock);
    return TRUE;
}

// ShowDamageNumber runs on the game thread and the draw path on the render
// thread. The old lazy check let both enter the initialiser at once, which
// handed one of them a critical section that was still being built.
// InitOnceExecuteOnce makes every caller wait for the callback.
static void FloaterInit() {
    InitOnceExecuteOnce(&g_fltOnce, FloaterOnce, nullptr, nullptr);
}

void ShowDamageNumber(void* target, long long dmg) {
    if (!target || dmg <= 0) return;
    void* ts = FieldPtr(target, "<tsTransform>k__BackingField");
    if (!ts) return;
    void* ut = *(void**)((uint8_t*)ts + 0x48);
    if (!ut) return;
    static void* camClass = nullptr;
    if (!camClass) {
        void* img = ImageByName("UnityEngine.CoreModule.dll");
        if (img && g_i2.class_from_name) camClass = g_i2.class_from_name(img, "UnityEngine", "Camera");
    }
    if (!camClass) return;
    void* exc = nullptr;
    void* mMain = Method(camClass, "get_main", 0);
    if (!mMain) return;
    void* cam = g_i2.runtime_invoke(mMain, nullptr, nullptr, &exc);
    if (exc || !cam) return;
    void* tc = g_i2.object_get_class(ut);
    void* mPos = Method(tc, "get_position", 0);
    if (!mPos) return;
    exc = nullptr;
    void* pos = g_i2.runtime_invoke(mPos, ut, nullptr, &exc);
    if (exc || !pos) return;
    struct V3 { float x, y, z; } v{ *(float*)((uint8_t*)pos + 0x10), *(float*)((uint8_t*)pos + 0x14), *(float*)((uint8_t*)pos + 0x18) };
    void* mW = Method(camClass, "WorldToScreenPoint", 1);
    if (!mW) return;
    void* args[1] = { &v };
    exc = nullptr;
    void* sp = g_i2.runtime_invoke(mW, cam, args, &exc);
    if (exc || !sp) return;
    float sx = *(float*)((uint8_t*)sp + 0x10);
    float sy = *(float*)((uint8_t*)sp + 0x14);
    if (sx <= 0 || sy <= 0 || sx > 8000 || sy > 8000) return;
    FloaterInit();
    EnterCriticalSection(&g_fltLock);
    int best = -1;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 64; i++) {
        if (g_floaters[i].until <= now) { best = i; break; }
    }
    if (best >= 0) {
        g_floaters[best].x = sx;
        g_floaters[best].y = sy;
        g_floaters[best].until = now + 900;
        if (dmg >= 1000000) _snprintf_s(g_floaters[best].text, sizeof(g_floaters[best].text), _TRUNCATE, "%.2fM", (double)dmg / 1000000.0);
        else if (dmg >= 1000) _snprintf_s(g_floaters[best].text, sizeof(g_floaters[best].text), _TRUNCATE, "%.1fK", (double)dmg / 1000.0);
        else _snprintf_s(g_floaters[best].text, sizeof(g_floaters[best].text), _TRUNCATE, "%lld", (long long)dmg);
    }
    LeaveCriticalSection(&g_fltLock);
}

static void DrawFloaters(ImDrawList* dl, const ImVec2& display) {
    FloaterInit();
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_fltLock);
    for (int i = 0; i < 64; i++) {
        if (g_floaters[i].until <= now) continue;
        ImVec2 p(g_floaters[i].x, display.y - g_floaters[i].y);
        dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 220), g_floaters[i].text);
        dl->AddText(p, IM_COL32(255, 214, 64, 255), g_floaters[i].text);
    }
    LeaveCriticalSection(&g_fltLock);
}


// ---------------------------------------------------------- overlay lifetime
// The detours stay enabled for the life of the process. Patching and
// unpatching live code from the hotkey thread while the game thread could be
// inside the detour is what crashed the process on menu close; the idle cost
// is paid instead by the early-out at the top of hkPresent.
static bool FloatersAlive() {
    FloaterInit();
    const ULONGLONG now = GetTickCount64();
    bool any = false;
    EnterCriticalSection(&g_fltLock);
    for (int i = 0; i < 64; i++) if (g_floaters[i].until > now) { any = true; break; }
    LeaveCriticalSection(&g_fltLock);
    return any;
}

// One physical press, one action. Polled rather than read from the window
// procedure because the subclass does not exist while the menu is closed.
static bool KeyPressed(int vk) {
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return edge;
}

static void CycleSpeed(const float* steps, int n, float* target) {
    int cur = 0;
    for (int i = 0; i < n; i++) if (*target >= steps[i] - 0.01f) cur = i;
    *target = steps[(cur + 1) % n];
}

DWORD WINAPI HotkeyThread(LPVOID) {
    static const float kGameSpeed[] = { 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f };
    static const float kMoveSpeed[] = { 1.0f, 1.2f, 1.5f, 2.0f, 3.0f, 5.0f };
    ULONGLONG lastSave = GetTickCount64();
    bool touched = false;

    // A just-resumed process can report a key as already held, which would read
    // as a press and open the menu on its own. Wait until the key is seen up
    // once before trusting an edge — but give up after two seconds so a stuck
    // report can never lock the menu out.
    const ULONGLONG armedBy = GetTickCount64() + 2000;
    bool armed = false;

    for (;;) {
        Sleep(16);

        if (!armed) {
            if (!(GetAsyncKeyState(VK_INSERT) & 0x8000)) {
                armed = true;
            } else if (GetTickCount64() > armedBy) {
                // Still reading as held: consume the state so the release is
                // what registers, and carry on rather than staying dark.
                KeyPressed(VK_INSERT);
                armed = true;
            } else {
                continue;
            }
        }

        if (KeyPressed(VK_INSERT)) {
            g_menuOpen = !g_menuOpen;
            LogF("menu %s", g_menuOpen ? "open" : "closed");
        }
        if (KeyPressed(VK_F1)) { g_cheat.god = !g_cheat.god; touched = true; }
        if (KeyPressed(VK_F2)) { g_cheat.infEnergy = !g_cheat.infEnergy; touched = true; }
        if (KeyPressed(VK_F3)) { g_cheat.dmgMult = g_cheat.dmgMult > 1 ? 1 : 10; touched = true; }
        if (KeyPressed(VK_F4)) { g_cheat.aura = !g_cheat.aura; touched = true; }
        // Not ResetSkillsNow(): that walks the IL2CPP heap, and this is not the
        // game thread. The action queue drains it inside the pcall hook, which
        // is. Calling it here tripped Unity's "Collecting from unknown thread".
        if (KeyPressed(VK_F5)) { QueueAction(ACT_RESET_SKILLS); touched = true; }
        if (KeyPressed(VK_F6)) { g_cheat.noCd = !g_cheat.noCd; touched = true; }
        if (KeyPressed(VK_F7)) { g_cheat.unlimitedDash = !g_cheat.unlimitedDash; touched = true; }
        if (KeyPressed(VK_F8)) { CycleSpeed(kMoveSpeed, 6, &g_cheat.moveSpdMult); touched = true; }
        if (KeyPressed(VK_F9)) { CycleSpeed(kGameSpeed, 7, &g_cheat.gameSpeed); touched = true; }

        // While the menu is open it owns the periodic save; this thread only
        // covers the closed case, so the two never write the file at once.
        const ULONGLONG now = GetTickCount64();
        if (!g_menuOpen && touched && now - lastSave >= 2000) {
            lastSave = now; touched = false; SaveConfig();
        }
    }
    return 0;
}


DWORD WINAPI InitThread(LPVOID) {
    while (!GetModuleHandleA("d3d11.dll")) Sleep(50);
    MH_Initialize();
    // Masked like the other literals: a registered class name is readable with
    // EnumWindows + GetClassName and would name the payload on sight.
    static constexpr auto kDummyClass = OBF("SSDummyWnd");
    const char* kClass = obf::Decoded(kDummyClass);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0, kClass, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx))) {
        g_status = "dummy device failed";
        return 0;
    }
    void** vt = *(void***)sc;
    g_presentAddr = vt[8];
    g_resizeAddr  = vt[13];
    sc->Release(); ctx->Release(); dev->Release(); DestroyWindow(dummy);
    UnregisterClassA(kClass, wc.hInstance);
    if (MH_CreateHook(g_presentAddr, (void*)&hkPresent, (void**)&g_origPresent) != MH_OK) {
        g_status = "MH_CreateHook failed";
        return 0;
    }
    if (MH_CreateHook(g_resizeAddr, (void*)&hkResizeBuffers, (void**)&g_origResizeBuffers) != MH_OK) {
        g_origResizeBuffers = nullptr;
        g_resizeAddr = nullptr;
    }
    // Enabled once, at startup, and never toggled again. hkPresent returns
    // straight through to the original while nothing is on screen, so the
    // overlay is inert without unpatching the game's own code mid-flight.
    if (g_resizeAddr) MH_EnableHook(g_resizeAddr);
    MH_EnableHook(g_presentAddr);
    LogF("present hook armed");
    return 0;
}
