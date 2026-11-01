// D3D11 present hook, theme, wallpaper and the ImGui menu.
#include "overlay.h"
#include "features.h"
#include "lua_bridge.h"
#include "il2cpp_api.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include "imgui.h"
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
static bool g_menuOpen = true;
using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
static Present_t g_origPresent = nullptr;
using ResizeBuffers_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffers_t g_origResizeBuffers = nullptr;
static int g_frame = 0;
static std::string g_status = "attaching...";


static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imguiReady) ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_INSERT) { g_menuOpen = !g_menuOpen; return 0; }
        if (wp == VK_F1) { g_cheat.god = !g_cheat.god; return 0; }
        if (wp == VK_F2) { g_cheat.infEnergy = !g_cheat.infEnergy; return 0; }
        if (wp == VK_F3) { g_cheat.dmgMult = g_cheat.dmgMult > 1 ? 1 : 10; return 0; }
        if (wp == VK_F4) { g_cheat.aura = !g_cheat.aura; return 0; }
        if (wp == VK_F6) { g_cheat.noCd = !g_cheat.noCd; return 0; }
        if (wp == VK_F9) {
            static const float gs[] = { 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f };
            int cur = 0;
            for (int i = 0; i < 7; i++) if (g_cheat.gameSpeed >= gs[i] - 0.01f) cur = i;
            g_cheat.gameSpeed = gs[(cur + 1) % 7];
            LogF("game speed %.2f", g_cheat.gameSpeed);
            return 0;
        }
        if (wp == VK_F12) { QueueDiag(); return 0; }
        if (wp == VK_F7) { g_cheat.unlimitedDash = !g_cheat.unlimitedDash; return 0; }
        if (wp == VK_F8) {
            static const float steps[] = { 1.0f, 1.2f, 1.5f, 2.0f, 3.0f, 5.0f };
            int cur = 0;
            for (int i = 0; i < 6; i++) if (g_cheat.moveSpdMult >= steps[i] - 0.01f) cur = i;
            g_cheat.moveSpdMult = steps[(cur + 1) % 6];
            LogF("move speed %.2f", g_cheat.moveSpdMult);
            return 0;
        }
        if (wp == VK_F5) { ResetSkillsNow(); return 0; }
    }
    if (g_menuOpen) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return 1;
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
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.ChildRounding = 6.0f;
    st.FrameRounding = 5.0f;
    st.GrabRounding = 5.0f;
    st.TabRounding = 5.0f;
    st.ScrollbarRounding = 8.0f;
    st.WindowBorderSize = 1.0f;
    st.FramePadding = ImVec2(10, 5);
    st.ItemSpacing = ImVec2(10, 7);
    st.CellPadding = ImVec2(6, 4);
    st.WindowPadding = ImVec2(14, 12);
    st.GrabMinSize = 12.0f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text]                  = ImVec4(1.00f, 0.94f, 0.88f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.78f, 0.55f, 0.44f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.025f, 0.015f, 0.60f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.03f, 0.02f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(0.95f, 0.38f, 0.10f, 0.55f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.06f, 0.03f, 0.80f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.35f, 0.11f, 0.05f, 0.90f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.50f, 0.16f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.12f, 0.03f, 0.02f, 0.85f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.22f, 0.06f, 0.03f, 0.95f);
    c[ImGuiCol_Button]                = ImVec4(0.32f, 0.09f, 0.04f, 0.90f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.62f, 0.19f, 0.06f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.85f, 0.30f, 0.08f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.55f, 0.18f, 0.06f, 0.80f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.78f, 0.28f, 0.08f, 0.90f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.95f, 0.38f, 0.10f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.95f, 0.40f, 0.12f, 0.45f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(1.00f, 0.55f, 0.20f, 0.80f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(1.00f, 0.65f, 0.25f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(1.00f, 0.52f, 0.12f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.95f, 0.35f, 0.10f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.64f, 0.20f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.20f, 0.06f, 0.03f, 0.85f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.75f, 0.25f, 0.08f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.66f, 0.20f, 0.06f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = ImVec4(1.00f, 0.62f, 0.18f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.14f, 0.04f, 0.02f, 0.85f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.40f, 0.12f, 0.05f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.06f, 0.02f, 0.01f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.55f, 0.18f, 0.06f, 0.90f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.80f, 0.28f, 0.08f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(1.00f, 0.45f, 0.12f, 1.00f);
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
    const wchar_t* path = L"C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\phoenix.jpg";
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

static void SectionHeader(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

static void LabeledSliderInt(const char* label, int* v, int lo, int hi, const char* fmt) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetItemTooltip("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    ImGui::SliderInt("##v", v, lo, hi, fmt);
    ImGui::PopID();
}

static void LabeledSliderFloat(const char* label, float* v, float lo, float hi) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetItemTooltip("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    ImGui::SliderFloat("##v", v, lo, hi, "%.1f");
    ImGui::PopID();
}

static void SectionHeaderRow(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.15f, 1.00f), "%s", text);
}

static void CheckboxRow(const char* label, bool* v) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetItemTooltip("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    ImGui::Checkbox("##c", v);
    ImGui::PopID();
}

static bool AceLoaded() {
    const char* names[] = { "ACE-Base64.dll", "ACE-Base.dll", "ACE-CORE.dll", "ACE-GuardClient.dll", "ACE-Service64.exe" };
    for (const char* n : names) if (GetModuleHandleA(n)) return true;
    return false;
}

static bool AceInstalled() {
    return GetFileAttributesA("C:\\YostarGames\\StellaSora_EN\\AntiCheatExpert") != INVALID_FILE_ATTRIBUTES;
}


static void DrawMenu() {
    if (g_cfgX >= 0) ImGui::SetNextWindowPos(ImVec2(g_cfgX, g_cfgY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(g_cfgW, g_cfgH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640, 360), ImVec2(1600, 1000));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("Stella Sora Cheat", &g_menuOpen);
    {
        ImVec2 p0 = ImGui::GetWindowPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_wallTex) {
            dl->AddImage((ImTextureID)(intptr_t)g_wallTex, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, g_wallAlpha));
        }
        dl->AddRectFilled(p0, p1, g_wallTex ? IM_COL32(0, 0, 0, g_dimAlpha) : IM_COL32(12, 12, 16, 235));
    }
    ImGui::TextColored(ImVec4(1.00f, 0.48f, 0.12f, 1.00f), "Stella Sora Cheat");
    ImGui::SameLine();
    ImGui::TextDisabled("by Azels56   v2.0");
    ImGui::TextDisabled("INSERT menu   F1 God   F2 Energy   F3 Damage   F4 Aura   F5 Reset   F6 No-CD   F7 Dash   F8 Move   F9 Game speed");
    ImGui::Spacing();

    if (ImGui::BeginTabBar("cheat_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Combat")) {
            if (ImGui::BeginTable("combat", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                SectionHeaderRow("Multipliers");
                LabeledSliderInt("Damage", &g_cheat.dmgMult, 1, 100, "x%d");
                LabeledSliderInt("Defense", &g_cheat.defMult, 1, 100, "x%d");
                        LabeledSliderFloat("Game Speed", &g_cheat.gameSpeed, 1.0f, 5.0f);
        LabeledSliderFloat("Attack Speed", &g_cheat.atkSpdMult, 1.0f, 2.0f);
                LabeledSliderFloat("Move Speed", &g_cheat.moveSpdMult, 1.0f, 2.5f);
                SectionHeaderRow("Player");
                CheckboxRow("God Mode", &g_cheat.god);
                CheckboxRow("Always Critical Hit", &g_cheat.alwaysCrit);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Skills")) {
            if (ImGui::BeginTable("skills", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                SectionHeaderRow("Cooldowns");
                CheckboxRow("No Skill Cooldown", &g_cheat.noCd);
                CheckboxRow("Unlimited Energy", &g_cheat.infEnergy);
                CheckboxRow("Unlimited Dash", &g_cheat.unlimitedDash);
                SectionHeaderRow("Kill Aura");
                CheckboxRow("Kill Aura", &g_cheat.aura);
                CheckboxRow("One-shot Aura", &g_cheat.auraLethal);
                LabeledSliderInt("Aura Damage", &g_cheat.auraDmg, 10, 2000000000, "%d");
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("State")) {
            ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.20f, 1.00f), "Live status");
            ImGui::Separator();
            ImGui::Text("Players %d", g_lastPlayers);
            ImGui::Text("HP %lld / %lld   Alive %s", (long long)g_lastHp, (long long)g_lastMax, g_lastAlive ? "yes" : "no");
            ImGui::Text("Energy %lld", (long long)g_lastEnergy);
            ImGui::Text("Cooldown hook %s   Time scale %.2f", g_canUseCdFn ? "active" : "off", g_appliedTimeScale);
            ImGui::Text("Run speed %.2f", g_dbgRunSpeed);
            ImGui::Text("Aura %d hits   %lld -> %lld", g_auraHitsLast, (long long)g_auraHpBefore, (long long)g_auraHpAfter);
            ImGui::Spacing();
            ImGui::SeparatorText("Diagnostics");
            ImGui::BeginChild("diag", ImVec2(0, 180), true);
            {
                FILE* f = nullptr;
                if (fopen_s(&f, "C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\arena_diag.txt", "rb") == 0 && f) {
                    char buf[2048]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
                    ImGui::TextUnformatted(buf);
                } else {
                    ImGui::TextUnformatted("no data yet");
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Safety")) {
            if (ImGui::BeginTable("safety", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                SectionHeaderRow("Anti-cheat");
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("ACE base installed");
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(AceInstalled() ? ImVec4(1,0.6f,0.2f,1) : ImVec4(0.4f,1,0.4f,1), AceInstalled() ? "yes" : "no");
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("ACE module in game");
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(AceLoaded() ? ImVec4(1,0.6f,0.2f,1) : ImVec4(0.4f,1,0.4f,1), AceLoaded() ? "loaded" : "not loaded");
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Inline code patch");
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(g_canUseCdFn ? ImVec4(1,0.4f,0.3f,1) : ImVec4(0.4f,1,0.4f,1), g_canUseCdFn ? "active" : "none");
                SectionHeaderRow("Behaviour");
                CheckboxRow("Safe mode (no code hooks, no GM commands)", &g_cheat.safeMode);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Safe mode keeps every change in managed data only: no inline patches and no GM orders. "
                "Turn it off only if you need the stuck-enemy GM sweep.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Config")) {
            if (ImGui::BeginTable("config", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                SectionHeaderRow("Appearance");
                LabeledSliderInt("Wallpaper opacity", &g_wallAlpha, 0, 255, "%d");
                LabeledSliderInt("Backdrop darkness", &g_dimAlpha, 0, 255, "%d");
                LabeledSliderFloat("UI scale", &g_fontScale, 0.8f, 1.6f);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("Save config", ImVec2(140, 0))) { SaveConfig(); }
            ImGui::SameLine();
            if (ImGui::Button("Reload config", ImVec2(140, 0))) { LoadConfig(); }
            ImGui::SameLine();
            if (ImGui::Button("Defaults", ImVec2(110, 0))) {
                g_wallAlpha = 150; g_dimAlpha = 145; g_fontScale = 1.0f;
                g_cheat.dmgMult = 1; g_cheat.defMult = 1;
                g_cheat.atkSpdMult = 1.0f; g_cheat.moveSpdMult = 1.0f; g_cheat.gameSpeed = 1.0f;
                g_cheat.auraDmg = 5000; g_cheat.auraLethal = true;
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Config file:");
            ImGui::TextWrapped("%s", CFG_PATH);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset Skills", ImVec2(130, 0))) ResetSkillsNow();
    ImGui::SameLine();
    if (ImGui::Button("Reset Stats", ImVec2(120, 0))) ResetModifiers();
    ImGui::SameLine();
    if (ImGui::Button("Kill Mobs", ImVec2(120, 0))) KillMobs();

    {
        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 sz = ImGui::GetWindowSize();
        g_cfgX = p.x; g_cfgY = p.y; g_cfgW = sz.x; g_cfgH = sz.y;
        static ULONGLONG lastSave = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastSave >= 2000) { lastSave = now; SaveConfig(); }
    }
    ImGui::End();
}

static void RenderOverlay(IDXGISwapChain* sc) {
    __try {
        if (!g_ctx) return;
        if (g_dev && !g_wallTex) LoadWallpaper(g_dev);
        CreateRTV(sc);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
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
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return g_origPresent(sc, sync, flags);

    // Unity recreates the swapchain/device when entering or leaving a match.
    // Never render with a stale device, context or back-buffer view.
    if (g_imguiReady && (dev != g_dev || sc != g_swap)) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_rtvSwap = nullptr;
        ImGui_ImplDX11_Shutdown();
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
        g_dev = dev;
        dev = nullptr;
        g_swap = sc;
        g_dev->GetImmediateContext(&g_ctx);
        if (g_ctx) ImGui_ImplDX11_Init(g_dev, g_ctx);
    }

    if (!g_imguiReady) {
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
                } else {
                    ImFontConfig fc;
                    fc.SizePixels = 20.0f;
                    io.Fonts->AddFontDefault(&fc);
                }
                ImGui::StyleColorsDark();
                ApplyTheme();
                ImGui_ImplWin32_Init(g_hwnd);
                ImGui_ImplDX11_Init(g_dev, g_ctx);
                g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
                g_imguiReady = true;
            }
        }
    }
    if (dev) dev->Release();

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
static bool g_fltInit = false;

static void FloaterInit() {
    if (!g_fltInit) { InitializeCriticalSection(&g_fltLock); g_fltInit = true; }
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
    if (!g_fltInit) return;
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


DWORD WINAPI InitThread(LPVOID) {
    while (!GetModuleHandleA("d3d11.dll")) Sleep(50);
    MH_Initialize();
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "SSDummyWnd";
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0, "SSDummyWnd", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
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
    void* present = vt[8];
    void* resize = vt[13];
    sc->Release(); ctx->Release(); dev->Release(); DestroyWindow(dummy);
    if (MH_CreateHook(present, (void*)&hkPresent, (void**)&g_origPresent) != MH_OK) {
        g_status = "MH_CreateHook failed";
        return 0;
    }
    MH_EnableHook(present);
    if (MH_CreateHook(resize, (void*)&hkResizeBuffers, (void**)&g_origResizeBuffers) == MH_OK) {
        MH_EnableHook(resize);
    }
    return 0;
}
