// Menu.cpp — STELLA SORA overlay shell.
//
//   +--------------------------------------------------------------+
//   |  STELLA SORA            by Azels56 · v2.0                 [x] |
//   +----------+---------------------------------------------------+
//   |  Combat  |  | Multipliers   applied on every tick            |
//   |  Skills  |    Damage                        [==== x36 ====]  |
//   |  Status  |    Defense                       [===== x1 ====]  |
//   |  Script  |  | Player        survivability                    |
//   |  Safety  |    God Mode                            (toggle)    |
//   |  Logs    |                                                   |
//   | Settings |                                                   |
//   +----------+---------------------------------------------------+
//   | Reset Skills   Reset Stats   Kill Enemies                     |
//   | INSERT toggles the menu           * ACE loaded  * live  60 fps|
//   +--------------------------------------------------------------+
//
// Everything is drawn with the window draw list plus a handful of invisible
// buttons, so the rows can be laid out left-aligned and the controls pinned to
// the right edge instead of following ImGui's default flow.

#include "Menu.h"
#include "features.h"
#include "overlay.h"
#include "il2cpp_api.h"
#include "luau_bridge.h"
#include "str_obf.h"

#include <windows.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

extern bool g_menuOpen;

namespace {

constexpr float kPi = 3.14159265f;

// Masked so a string scan of the loaded module does not find the payload's own
// filenames sitting in .rdata.
static constexpr auto kCfgLabel  = OBF("work\\stella_menu.ini");
static constexpr auto kLuauLabel = OBF("work\\luau_run.lua  \xc2\xb7  game thread");
static constexpr auto kMenuId    = OBF("##stella_menu");

// ImGui keeps the pointer it is given, so the id has to outlive the call.
const char* MenuWindowId() { static const auto d = obf::Decoded(kMenuId); return d; }

// ---------------------------------------------------------------- palette

constexpr ImU32 kColTitle  = IM_COL32(248, 243, 237, 255);
constexpr ImU32 kColText   = IM_COL32(228, 221, 214, 255);
constexpr ImU32 kColMuted  = IM_COL32(146, 137, 128, 255);
// 138,130,122 clears 4.5:1 on the card surface; the old 102,95,89 sat at
// 2.9:1 and was the only palette entry failing the contrast floor.
constexpr ImU32 kColFaint  = IM_COL32(138, 130, 122, 255);
constexpr ImU32 kColAccent = IM_COL32(255, 138,  36, 255);
constexpr ImU32 kColIdle   = IM_COL32(152, 144, 136, 255);
constexpr ImU32 kColTrack  = IM_COL32(56,  48,  43, 255);
constexpr ImU32 kColGreen  = IM_COL32(120, 205, 130, 255);

// ---------------------------------------------------------------- metrics

constexpr float kHeaderH = 78.0f;
constexpr float kRailW   = 214.0f;
constexpr float kActionH = 50.0f;
constexpr float kFooterH = 44.0f;
constexpr float kTabX    = 14.0f;
constexpr float kTabW    = 186.0f;
constexpr float kTabH    = 50.0f;
constexpr float kTabGap  = 4.0f;
constexpr float kCardPad = 14.0f;

// UI Scale from the settings tab multiplies every pixel size below.
float g_uiScale = 1.0f;
inline float S(float v) { return v * g_uiScale; }

ImFont* BoldFont() { return g_fontBold ? g_fontBold : ImGui::GetFont(); }

ImU32 LerpCol(ImU32 a, ImU32 b, float t)
{
    const int ar = (int)((a >> IM_COL32_R_SHIFT) & 0xFF), ag = (int)((a >> IM_COL32_G_SHIFT) & 0xFF);
    const int ab = (int)((a >> IM_COL32_B_SHIFT) & 0xFF), aa = (int)((a >> IM_COL32_A_SHIFT) & 0xFF);
    const int br = (int)((b >> IM_COL32_R_SHIFT) & 0xFF), bg = (int)((b >> IM_COL32_G_SHIFT) & 0xFF);
    const int bb = (int)((b >> IM_COL32_B_SHIFT) & 0xFF), ba = (int)((b >> IM_COL32_A_SHIFT) & 0xFF);
    return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t),
                    (int)(ab + (bb - ab) * t), (int)(aa + (ba - aa) * t));
}

// Scales a colour's alpha, for the muted treatment a disabled control uses.
inline ImU32 Dim32(ImU32 c, float a)
{
    const ImU32 al = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

// Small per-ID value cache so toggles ease instead of snapping. Fixed size: the
// menu has far fewer animated widgets than slots.
float Anim(ImGuiID id, float target, float speed)
{
    struct Slot { ImGuiID id; float v; bool used; };
    static Slot slots[64] = {};
    static int  count = 0;

    Slot* s = nullptr;
    for (int i = 0; i < count; ++i)
        if (slots[i].used && slots[i].id == id) { s = &slots[i]; break; }
    if (!s && count < 64) { s = &slots[count++]; s->id = id; s->v = target; s->used = true; }
    if (!s) return target;

    const float dt = fminf(0.05f, ImGui::GetIO().DeltaTime);
    s->v += (target - s->v) * fminf(1.0f, dt * speed);
    if (fabsf(target - s->v) < 0.002f) s->v = target;
    return s->v;
}

// ---------------------------------------------------------------- paths

void PathCircle(ImDrawList* dl, ImVec2 c, float r, int segments = 40)
{
    for (int i = 0; i <= segments; ++i)
    {
        const float a = static_cast<float>(i) * (2.0f * kPi / static_cast<float>(segments));
        dl->PathLineTo(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r));
    }
}

void PathBezier(ImDrawList* dl, ImVec2 p0, ImVec2 ctrl, ImVec2 p1, int steps = 14)
{
    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        dl->PathLineTo(ImVec2(u * u * p0.x + 2.0f * u * t * ctrl.x + t * t * p1.x,
                              u * u * p0.y + 2.0f * u * t * ctrl.y + t * t * p1.y));
    }
}

// ---------------------------------------------------------------- icons
//
// Every icon is authored in a +-11 unit box around c, then scaled.

inline ImVec2 P(ImVec2 c, float s, float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); }

void IconBolt(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    dl->PathLineTo(P(c, s,  1.5f, -11.0f));
    dl->PathLineTo(P(c, s, -6.5f,   1.5f));
    dl->PathLineTo(P(c, s, -0.5f,   1.5f));
    dl->PathLineTo(P(c, s, -1.5f,  11.0f));
    dl->PathLineTo(P(c, s,  6.5f,  -2.0f));
    dl->PathLineTo(P(c, s,  0.5f,  -2.0f));
    dl->PathStroke(col, ImDrawFlags_Closed, 1.6f * s);
}

void IconSpark(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    dl->PathLineTo(P(c, s,  0.0f, -11.0f));
    dl->PathLineTo(P(c, s,  2.2f,  -2.2f));
    dl->PathLineTo(P(c, s, 11.0f,   0.0f));
    dl->PathLineTo(P(c, s,  2.2f,   2.2f));
    dl->PathLineTo(P(c, s,  0.0f,  11.0f));
    dl->PathLineTo(P(c, s, -2.2f,   2.2f));
    dl->PathLineTo(P(c, s, -11.0f,  0.0f));
    dl->PathLineTo(P(c, s, -2.2f,  -2.2f));
    dl->PathStroke(col, ImDrawFlags_Closed, 1.5f * s);
}

void IconChart(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float t = 1.5f * s;
    dl->AddLine(P(c, s, -9.0f, 9.0f), P(c, s, 9.0f, 9.0f), col, t);
    dl->AddRectFilled(P(c, s, -7.0f, 1.0f), P(c, s, -3.5f, 9.0f), col, 1.2f * s);
    dl->AddRectFilled(P(c, s, -2.0f, -5.0f), P(c, s, 1.5f, 9.0f), col, 1.2f * s);
    dl->AddRectFilled(P(c, s, 3.0f, -1.5f), P(c, s, 6.5f, 9.0f), col, 1.2f * s);
}

void IconShield(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float t = 1.5f * s;
    dl->PathLineTo(P(c, s, 0.0f, -11.0f));
    dl->PathLineTo(P(c, s, 8.5f, -7.0f));
    dl->PathLineTo(P(c, s, 8.5f, 0.0f));
    PathBezier(dl, P(c, s, 8.5f, 0.0f), P(c, s, 8.5f, 7.0f), P(c, s, 0.0f, 11.0f));
    PathBezier(dl, P(c, s, 0.0f, 11.0f), P(c, s, -8.5f, 7.0f), P(c, s, -8.5f, 0.0f));
    dl->PathLineTo(P(c, s, -8.5f, -7.0f));
    dl->PathStroke(col, ImDrawFlags_Closed, t);
}

void IconList(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float t = 1.5f * s;
    dl->AddLine(P(c, s, -9.0f, -6.5f), P(c, s, 9.0f, -6.5f), col, t);
    dl->AddLine(P(c, s, -9.0f,  0.0f), P(c, s, 6.0f,  0.0f), col, t);
    dl->AddLine(P(c, s, -9.0f,  6.5f), P(c, s, 8.0f,  6.5f), col, t);
}

void IconSliders(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float t = 1.4f * s;
    dl->AddLine(P(c, s, -9.0f, -7.0f), P(c, s, 9.0f, -7.0f), col, t);
    dl->AddCircleFilled(P(c, s, 3.5f, -7.0f), 2.4f * s, col);
    dl->AddLine(P(c, s, -9.0f, 0.0f), P(c, s, 9.0f, 0.0f), col, t);
    dl->AddCircleFilled(P(c, s, -3.5f, 0.0f), 2.4f * s, col);
    dl->AddLine(P(c, s, -9.0f, 7.0f), P(c, s, 9.0f, 7.0f), col, t);
    dl->AddCircleFilled(P(c, s, 4.0f, 7.0f), 2.4f * s, col);
}

void IconCode(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float t = 1.5f * s;
    // Left chevron.
    dl->AddLine(P(c, s, -3.0f, -8.0f), P(c, s, -9.0f, 0.0f), col, t);
    dl->AddLine(P(c, s, -9.0f, 0.0f), P(c, s, -3.0f, 8.0f), col, t);
    // Right chevron.
    dl->AddLine(P(c, s, 3.0f, -8.0f), P(c, s, 9.0f, 0.0f), col, t);
    dl->AddLine(P(c, s, 9.0f, 0.0f), P(c, s, 3.0f, 8.0f), col, t);
}

void IconPlay(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // Rounded triangle: the rail reads it as "run", not as a warning sign.
    dl->PathLineTo(P(c, s, -6.5f, -10.0f));
    dl->PathLineTo(P(c, s, 9.5f,   0.0f));
    dl->PathLineTo(P(c, s, -6.5f,  10.0f));
    dl->PathStroke(col, ImDrawFlags_Closed, 1.6f * s);
}

void DrawIcon(ImDrawList* dl, int index, ImVec2 c, float s, ImU32 col)
{
    switch (index)
    {
        case 0: IconBolt(dl, c, s, col);    break;
        case 1: IconPlay(dl, c, s, col);    break;
        case 2: IconSpark(dl, c, s, col);   break;
        case 3: IconChart(dl, c, s, col);   break;
        case 4: IconCode(dl, c, s, col);    break;
        case 5: IconShield(dl, c, s, col);  break;
        case 6: IconList(dl, c, s, col);    break;
        default: IconSliders(dl, c, s, col); break;
    }
}

// ---------------------------------------------------------------- tabs

struct TabDef { const char* label; };

const TabDef kTabs[] = {
    { "Combat" }, { "Game" }, { "Skills" }, { "Status" }, { "Script" },
    { "Safety" }, { "Logs" }, { "Settings" },
};
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

bool DrawTab(ImDrawList* dl, ImVec2 p, int index, bool selected, float tabH = kTabH)
{
    const ImVec2 sz(kTabW, tabH);

    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton(kTabs[index].label, sz);
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();

    if (selected)
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(255, 138, 36, 30), S(7.0f));
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(255, 255, 255, 12), S(7.0f));

    if (selected)
        dl->AddRectFilled(ImVec2(p.x, p.y + 13.0f), ImVec2(p.x + 3.0f, p.y + sz.y - 13.0f),
                          kColAccent, 1.5f);

    const ImU32 col = selected ? kColAccent : (hovered ? kColText : kColIdle);
    DrawIcon(dl, index, ImVec2(p.x + S(28.0f), p.y + sz.y * 0.5f), 1.0f, col);

    // Left-aligned, never centred: the icon column stays put so the labels line
    // up down the rail.
    const float px = ImGui::GetFontSize();
    dl->AddText(ImGui::GetFont(), px,
                ImVec2(p.x + S(50.0f), p.y + (sz.y - px) * 0.5f), col, kTabs[index].label);

    return clicked;
}

// ---------------------------------------------------------------- cards
//
// A card is a rounded panel holding a header and a stack of rows. The panel is
// drawn after its rows are known, into a separate draw-list channel, so the
// background always lands behind the content.

struct Card
{
    ImDrawListSplitter split;
    ImDrawList* dl = nullptr;
    ImVec2 start{};
    float  w = 0.0f;
    bool   open = false;
};

float g_rowW = 400.0f;
ImVec2 g_origin{ 0.0f, 0.0f }; // top-left of the content child, screen space

void CardBegin(Card& c, const char* title, const char* desc, float width = 0.0f)
{
    c.dl    = ImGui::GetWindowDrawList();
    c.start = ImGui::GetCursorScreenPos();
    c.w     = (width > 0.0f) ? width : ImGui::GetContentRegionAvail().x - S(6.0f);
    c.split.Split(c.dl, 2);
    c.split.SetCurrentChannel(c.dl, 1);
    c.open  = true;

    // Reserve the header strip with a real item so the cursor stays inside the
    // window's known content box (otherwise ImGui raises a boundary error).
    ImGui::SetCursorScreenPos(c.start);
    ImGui::Dummy(ImVec2(c.w, S(44.0f)));
    const float x = c.start.x + kCardPad;
    const float y = c.start.y + S(13.0f);

    c.dl->AddRectFilled(ImVec2(x, y + 1.0f), ImVec2(x + 3.0f, y + S(20.0f)), kColAccent, 1.5f);

    ImFont* fb = BoldFont();
    const float hpx = S(17.0f);
    c.dl->AddText(fb, hpx, ImVec2(x + 12.0f, y), kColText, title);
    if (desc && *desc)
    {
        // The subtitle is a deliberate size step down from the title. Drawn at
        // body size it outweighed the 17px heading and the pair read as one
        // run-on sentence instead of a heading with a caption.
        const float dpx = S(13.0f);
        const float tw  = fb->CalcTextSizeA(hpx, FLT_MAX, 0.0f, title).x;
        const float dx  = x + 12.0f + tw + S(10.0f);
        const float fit = (c.start.x + c.w - kCardPad) - dx;
        const float dy  = y + (hpx - dpx) * 0.5f + 1.0f;

        // Subtitles are decoration: trim with an ellipsis rather than spill
        // past the card edge when a narrow card leaves no room.
        if (fit > 0.0f)
        {
            ImFont* f = ImGui::GetFont();
            char tmp[128];
            strncpy_s(tmp, desc, _TRUNCATE);
            if (f->CalcTextSizeA(dpx, FLT_MAX, 0.0f, tmp).x > fit)
            {
                char probe[136];
                probe[0] = '\0';
                for (size_t n = strlen(tmp); n > 0; )
                {
                    tmp[--n] = 0;
                    _snprintf_s(probe, _TRUNCATE, "%s\xe2\x80\xa6", tmp);
                    if (f->CalcTextSizeA(dpx, FLT_MAX, 0.0f, probe).x <= fit) break;
                }
                c.dl->AddText(f, dpx, ImVec2(dx, dy), kColMuted, probe);
            }
            else
            {
                c.dl->AddText(f, dpx, ImVec2(dx, dy), kColMuted, tmp);
            }
        }
    }

    g_rowW = c.w - kCardPad * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, c.start.y + S(44.0f)));
}

void CardEnd(Card& c, float minBottom = 0.0f)
{
    if (!c.open) return;

    // Two cards side by side rarely end on the same line. A caller that knows
    // the taller column's bottom pads this one so the pair reads as a row
    // instead of a staircase.
    if (minBottom > 0.0f) {
        const float y = ImGui::GetCursorScreenPos().y;
        const float target = minBottom - S(12.0f);
        if (y < target) ImGui::Dummy(ImVec2(1.0f, target - y));
    }

    const ImVec2 end = ImGui::GetCursorScreenPos();

    // Bottom padding as an item. The cursor is left where it lands; the next
    // card only rewinds x, which is always legal.
    ImGui::SetCursorScreenPos(ImVec2(c.start.x + kCardPad, end.y));
    ImGui::Dummy(ImVec2(g_rowW, S(12.0f)));

    c.split.SetCurrentChannel(c.dl, 0);

    const ImVec2 p0(c.start.x, c.start.y);
    const ImVec2 p1(c.start.x + c.w, end.y + S(12.0f));
    c.dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 13), S(8.0f));
    c.dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 20), S(8.0f));

    c.split.Merge(c.dl);
    c.open = false;
}

// ---------------------------------------------------------------- rows

struct RowGeom { ImVec2 p; float w; float h; };

RowGeom RowStart(const char* label, bool hasDesc)
{
    RowGeom g;
    g.p = ImGui::GetCursorScreenPos();
    g.w = g_rowW;
    g.h = S(hasDesc ? 58.0f : 46.0f);
    (void)label;
    // A real item of the full row size, so the child's content box grows with
    // the rows and the cursor never jumps past it.
    ImGui::Dummy(ImVec2(g.w, g.h));
    return g;
}

void RowEnd(const RowGeom& g) { ImGui::SetCursorScreenPos(ImVec2(g.p.x, g.p.y + g.h)); }

// Trims text to maxW with a trailing ellipsis. Returns a pointer the caller can
// draw: either the original string or buf. Labels used to be drawn unclamped,
// so on a narrow card the description ran underneath the slider.
const char* FitText(ImFont* font, float px, const char* text, float maxW, char* buf, size_t cap)
{
    if (!text || !*text || maxW <= S(8.0f)) return nullptr;
    if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x <= maxW) return text;

    strncpy_s(buf, cap, text, _TRUNCATE);
    char probe[192];
    for (size_t n = strlen(buf); n > 0; )
    {
        buf[--n] = 0;
        _snprintf_s(probe, _TRUNCATE, "%s\xe2\x80\xa6", buf);
        if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, probe).x <= maxW)
        {
            strncpy_s(buf, cap, probe, _TRUNCATE);
            return buf;
        }
    }
    buf[0] = 0;
    return nullptr;
}

// Returns true when the label or description had to be trimmed, so the caller
// can put the full string in a hover tooltip instead of losing it silently.
bool RowLabels(ImDrawList* dl, const RowGeom& g, const char* label, const char* desc, bool dim,
               float maxX = FLT_MAX)
{
    const float px = ImGui::GetFontSize();
    const ImU32 col = dim ? kColFaint : kColText;
    const float fit = (maxX == FLT_MAX) ? g.w : (maxX - g.p.x);

    char lb[96] = { 0 }, db[192] = { 0 };
    const char* l = FitText(ImGui::GetFont(), px, label, fit, lb, sizeof(lb));
    const char* d = desc ? FitText(ImGui::GetFont(), S(14.0f), desc, fit, db, sizeof(db)) : nullptr;

    if (l)
        dl->AddText(ImGui::GetFont(), px,
                    ImVec2(g.p.x, desc ? g.p.y + S(8.0f) : g.p.y + (g.h - px) * 0.5f), col, l);
    if (d)
        dl->AddText(ImGui::GetFont(), S(14.0f), ImVec2(g.p.x, g.p.y + S(30.0f)), kColMuted, d);

    const bool labelCut = (label && *label) && (l != label);
    const bool descCut  = (desc && *desc) && (d != desc);
    return labelCut || descCut;
}

// enabled=false renders the switch in its muted "cannot act yet" state and
// blocks interaction; reason is shown on hover so the row explains itself.
bool ToggleRow(const char* label, const char* desc, bool* v, bool enabled = true,
               const char* reason = nullptr)
{
    ImGui::PushID(label);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const RowGeom g = RowStart(label, desc != nullptr);

    const ImVec2 tsz(S(46.0f), S(24.0f));
    const ImVec2 tp(g.p.x + g.w - tsz.x, g.p.y + (g.h - tsz.y) * 0.5f);

    ImGui::SetCursorScreenPos(tp);
    const bool pressed = ImGui::InvisibleButton("##tg", tsz,
                              enabled ? ImGuiButtonFlags_EnableNav : ImGuiButtonFlags_None);
    if (enabled && pressed) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();

    if (!enabled)
    {
        if (hovered && reason) ImGui::SetTooltip("%s", reason);
    }

    const float t = Anim(ImGui::GetID("##tg"), *v ? 1.0f : 0.0f, 18.0f);
    ImU32 track = LerpCol(kColTrack, kColAccent, t);
    if (!enabled)          track = Dim32(kColTrack, 0.75f);
    else if (hovered)      track = LerpCol(track, IM_COL32(255, 255, 255, 255), 0.12f);

    dl->AddRectFilled(tp, ImVec2(tp.x + tsz.x, tp.y + tsz.y), track, tsz.y * 0.5f);

    const float kr = (tsz.y - S(6.0f)) * 0.5f;
    const float kx = tp.x + S(3.0f) + kr + t * (tsz.x - S(6.0f) - 2.0f * kr);
    const ImU32 knob = enabled ? IM_COL32(250, 246, 242, 255) : IM_COL32(150, 144, 138, 255);
    dl->AddCircleFilled(ImVec2(kx, tp.y + tsz.y * 0.5f), kr, knob);

    const bool cut = RowLabels(dl, g, label, desc, !enabled, tp.x - S(10.0f));
    if (enabled && hovered && cut && reason == nullptr) ImGui::SetTooltip("%s", label);
    RowEnd(g);

    ImGui::PopID();
    return false;
}

// Slider drawn by hand: dark track, orange fill from the left, a grab handle at
// the current position and the value in its own column to the right. The handle
// is what makes the control read as a slider at the minimum, where the fill has
// no width and the old version looked like an empty text field.
bool SliderRow(const char* label, const char* desc, void* val, bool isInt,
               float lo, float hi, const char* fmt, bool enabled = true,
               const char* reason = nullptr)
{
    ImGui::PushID(label);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const RowGeom g = RowStart(label, desc != nullptr);

    // The readout lives outside the pill. Anything sharing the pill's width
    // steals travel from the track, which left the handle parked ~78% of the
    // way along at the maximum and made a full value look half-set.
    const float valueW = S(56.0f);
    const float gap    = S(10.0f);
    const float sw = fminf(S(300.0f), g.w * 0.42f);
    const float sh = S(28.0f);
    const ImVec2 sp(g.p.x + g.w - valueW - gap - sw, g.p.y + (g.h - sh) * 0.5f);

    ImGui::SetCursorScreenPos(sp);
    ImGui::InvisibleButton("##sl", ImVec2(sw, sh),
                           enabled ? ImGuiButtonFlags_EnableNav : ImGuiButtonFlags_None);
    const bool active  = enabled && ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = enabled && ImGui::IsItemFocused();

    if (!enabled && hovered && reason) ImGui::SetTooltip("%s", reason);

    const float step = isInt ? 1.0f : (hi - lo) / 100.0f;

    if (active)
    {
        const float mx = ImGui::GetIO().MousePos.x;
        float t = (mx - sp.x) / sw;
        t = fmaxf(0.0f, fminf(1.0f, t));
        const float nv = lo + t * (hi - lo);
        if (isInt)
        {
            const int iv = (int)lroundf(nv);
            if (iv != *(int*)val) *(int*)val = iv;
        }
        else
        {
            if (fabsf(nv - *(float*)val) > 1e-6f) *(float*)val = nv;
        }
    }
    else if (focused)
    {
        // Keyboard path: arrows nudge by one integer step, or 1% of the range
        // for floats. Held keys repeat through ImGui's own repeat rate.
        float dir = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  dir -= 1.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dir += 1.0f;
        if (dir != 0.0f)
        {
            if (isInt)
            {
                int iv = (int)lroundf((float)*(int*)val + dir * step);
                *(int*)val = (int)fmaxf(lo, fminf(hi, (float)iv));
            }
            else
            {
                *(float*)val = fmaxf(lo, fminf(hi, *(float*)val + dir * step));
            }
        }
    }

    const float cur = isInt ? (float)*(int*)val : *(float*)val;
    float t = (hi > lo) ? (cur - lo) / (hi - lo) : 0.0f;
    t = fmaxf(0.0f, fminf(1.0f, t));

    dl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + sh), IM_COL32(58, 48, 42, 255), S(7.0f));

    // Handle travel is flush with the pill ends: at t=1 its right edge lands on
    // the pill's right edge, so a maximum value reads as a full control.
    const float kr = (sh - S(8.0f)) * 0.5f;
    const float kx = sp.x + kr + t * (sw - 2.0f * kr);

    if (kx - sp.x > 1.0f)
        dl->AddRectFilled(sp, ImVec2(kx, sp.y + sh),
                          enabled ? (hovered ? kColAccent : IM_COL32(232, 122, 30, 255))
                                  : Dim32(IM_COL32(232, 122, 30, 255), 0.45f),
                          S(7.0f));

    dl->AddCircleFilled(ImVec2(kx, sp.y + sh * 0.5f), kr,
                        enabled ? IM_COL32(250, 246, 242, 255) : IM_COL32(140, 134, 128, 255));

    char buf[64];
    if (isInt) _snprintf_s(buf, _TRUNCATE, fmt, *(int*)val);
    else       _snprintf_s(buf, _TRUNCATE, fmt, *(float*)val);

    // Readout sits in its own column past the pill, so the track always runs
    // edge to edge and a maximum value paints the whole control.
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(g.p.x + g.w - ts.x, sp.y + (sh - ts.y) * 0.5f),
                enabled ? kColText : kColFaint, buf);

    const bool cut = RowLabels(dl, g, label, desc, !enabled, sp.x - S(12.0f));
    if (enabled && hovered && cut) ImGui::SetTooltip("%s", label);
    RowEnd(g);

    ImGui::PopID();
    return false;
}

void InfoRow(const char* label, const char* value, ImU32 valueCol = kColMuted)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const RowGeom g = RowStart(label, false);

    const float px = ImGui::GetFontSize();
    const ImVec2 ts = ImGui::CalcTextSize(value);
    const float vx = g.p.x + g.w - ts.x;

    char lb[96] = { 0 };
    if (const char* l = FitText(ImGui::GetFont(), px, label, vx - g.p.x - S(16.0f), lb, sizeof(lb)))
        dl->AddText(ImGui::GetFont(), px, ImVec2(g.p.x, g.p.y + (g.h - px) * 0.5f), kColText, l);

    dl->AddText(ImGui::GetFont(), px,
                ImVec2(vx, g.p.y + (g.h - px) * 0.5f), valueCol, value);

    RowEnd(g);
}

bool CardButton(const char* label, float w)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.12f, 0.09f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.30f, 0.08f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.54f, 0.14f, 0.95f));
    const bool r = ImGui::Button(label, ImVec2(w, S(30.0f)));
    ImGui::PopStyleColor(3);
    return r;
}

// ---------------------------------------------------------------- tabs

void CombatTab()
{
    Card c;

    // Two columns: five sliders on the left already fill a 600px pane, so the
    // toggles sit beside them instead of scrolling off the bottom.
    const float contentW = ImGui::GetContentRegionAvail().x - S(6.0f);
    const float gap      = S(12.0f);
    const float leftW    = (contentW - gap) * 0.53f;
    const float rightW   = contentW - gap - leftW;

    const ImVec2 rowTop = ImGui::GetCursorScreenPos();

    CardBegin(c, "Multipliers", "applied on every tick", leftW);
    // No sub-labels here: five of them would push this card past the pane at the
    // minimum window height, and a half-labelled column reads as a mistake.
    SliderRow("Damage", nullptr, &g_cheat.dmgMult, true, 1.0f, 50.0f, "x%d");
    SliderRow("Defense", nullptr, &g_cheat.defMult, true, 1.0f, 100.0f, "x%d");
    SliderRow("Attack Speed", nullptr, &g_cheat.atkSpdMult, false, 1.0f, 2.0f, "%.2fx");
    SliderRow("Game Speed", nullptr, &g_cheat.gameSpeed, false, 1.0f, 5.0f, "%.2fx");
    SliderRow("Move Speed", nullptr, &g_cheat.moveSpdMult, false, 1.0f, 2.5f, "%.2fx");
    CardEnd(c);

    ImGui::SetCursorScreenPos(ImVec2(rowTop.x + leftW + gap, rowTop.y));

    CardBegin(c, "Player", "survivability", rightW);
    ToggleRow("God Mode", "Ignores all incoming damage.", &g_cheat.god);
    ToggleRow("Always Critical", "Every hit lands as a crit.", &g_cheat.alwaysCrit);
    CardEnd(c);

    // CardEnd leaves the cursor at the content origin, so the column has to be
    // re-anchored before the next card or it falls back to the left side.
    const float playerBottom = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(rowTop.x + leftW + gap, playerBottom));

    CardBegin(c, "Hits", "break, shield, repeats", rightW);
    ToggleRow("Instant Break", nullptr, &g_cheat.instantBreak);
    ToggleRow("Remove Shield", nullptr, &g_cheat.noShield);
    SliderRow("Multi-Hit", nullptr, &g_cheat.multiHit, true, 1.0f, 8.0f, "%d\xc3\x97");
    CardEnd(c);
}

void GameTab()
{
    Card c;

    const float contentW = ImGui::GetContentRegionAvail().x - S(6.0f);
    const float gap      = S(12.0f);
    const float leftW    = (contentW - gap) * 0.5f;
    const float rightW   = contentW - gap - leftW;

    const ImVec2 rowTop = ImGui::GetCursorScreenPos();

    // Right column first: its height is the taller of the two, and the left
    // card is padded to meet it so the pair ends on one line.
    ImGui::SetCursorScreenPos(ImVec2(rowTop.x + leftW + gap, rowTop.y));
    CardBegin(c, "Cards", "the choice picks itself", rightW);
    ToggleRow("Auto Select Card", "Takes the first option the moment it opens.",
              &g_cheat.autoSelectCards);
    InfoRow("Hook", "btnFateCard \xc2\xb7 btnConfirmReward");
    CardEnd(c);
    const float rowBottom = ImGui::GetCursorScreenPos().y;

    ImGui::SetCursorScreenPos(rowTop);

    CardBegin(c, "Story", "dialogs advance themselves", leftW);
    ToggleRow("Auto Skip Story", "Takes the skip path as soon as a dialog opens.",
              &g_cheat.autoSkipStory);
    InfoRow("Hook", "btnFoldMenu \xc2\xb7 btnSkip");
    CardEnd(c, rowBottom);

    // CardEnd leaves the cursor at the content origin, so the lower row has to
    // be anchored under the taller of the two columns.
    ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowBottom + S(4.0f)));

    CardBegin(c, "Discovery", "locks the exact names");
    InfoRow("Dump", "work\\automation_api.txt");
    if (CardButton("Scan game API", S(160.0f))) QueueAction(ACT_SCAN_AUTOMATION);
    CardEnd(c);
}

void SkillsTab()
{
    Card c;

    CardBegin(c, "Cooldowns", "skills never wait");
    ToggleRow("No Skill Cooldown", "Every skill is ready instantly.", &g_cheat.noCd);
    ToggleRow("Unlimited Energy", "Energy never drains.", &g_cheat.infEnergy);
    ToggleRow("Unlimited Dash", "Dashes cost nothing.", &g_cheat.unlimitedDash);
    CardEnd(c);

    CardBegin(c, "Aura", "sweeps everything around you");
    ToggleRow("Kill Aura", "Damages every enemy in range.", &g_cheat.aura);
    // The dependants stay on screen in a disabled state instead of appearing
    // and disappearing: the card keeps its height, and hovering the greyed rows
    // says what unlocks them.
    const char* kAuraGate = "Turn on Kill Aura first.";
    ToggleRow("One-Shot", "Skips the damage roll.", &g_cheat.auraLethal,
              g_cheat.aura, kAuraGate);
    SliderRow("Aura Damage", "Damage per sweep.", &g_cheat.auraDmg, true,
              10.0f, 2000000000.0f, "%d", g_cheat.aura, kAuraGate);
    CardEnd(c);
}

void StatusTab()
{
    Card c;
    char buf[96];

    const float contentW = ImGui::GetContentRegionAvail().x - S(6.0f);
    const float gap      = S(12.0f);
    const float leftW    = (contentW - gap) * 0.46f;
    const float rightW   = contentW - gap - leftW;

    const ImVec2 rowTop = ImGui::GetCursorScreenPos();

    CardBegin(c, "Live", "refreshed every tick", leftW);
    _snprintf_s(buf, _TRUNCATE, "%d", g_lastPlayers);
    InfoRow("Players", buf);
    _snprintf_s(buf, _TRUNCATE, "%lld / %lld", (long long)g_lastHp, (long long)g_lastMax);
    InfoRow("HP", buf, g_lastAlive ? kColGreen : kColAccent);
    _snprintf_s(buf, _TRUNCATE, "%lld", (long long)g_lastEnergy);
    InfoRow("Energy", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2fx", g_cheat.gameSpeed);
    InfoRow("Game speed", buf);
    CardEnd(c);

    ImGui::SetCursorScreenPos(ImVec2(rowTop.x + leftW + gap, rowTop.y));

    CardBegin(c, "Diagnostics", "feature state", rightW);
    _snprintf_s(buf, _TRUNCATE, "%d", g_auraHitsLast);
    InfoRow("Aura hits", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", g_auraFallbackHits);
    InfoRow("Fallback hits", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", g_dbgRunSpeed);
    InfoRow("Run speed", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", g_toughMonsters);
    InfoRow("Guard targets", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", g_multiHitExtra);
    InfoRow("Extra hits", buf);
    InfoRow("Lua bridge", g_luaScriptLoaded ? "loaded" : "pending",
            g_luaScriptLoaded ? kColGreen : kColMuted);
    CardEnd(c);
}

bool AceLoaded()
{
    const char* names[] = { "ACE-Base64.dll", "ACE-Base.dll", "ACE-CORE.dll",
                            "ACE-GuardClient.dll", "ACE-Service64.exe" };
    for (const char* n : names) if (GetModuleHandleA(n)) return true;
    return false;
}

bool AceInstalled()
{
    static constexpr auto kAceDir = OBF("C:\\YostarGames\\StellaSora_EN\\AntiCheatExpert");
    return GetFileAttributesA(obf::Decoded(kAceDir)) != INVALID_FILE_ATTRIBUTES;
}

void ScriptTab()
{
    // The bridge can enumerate any class and member, which the on-disk
    // resolution cache cannot answer, so the full graph is started here — on a
    // deliberate visit — instead of on every launch.
    EnsureMetadataGraph();

    // Seeded with a working example so the first Run does something visible.
    static char src[8192] =
        "-- Luau runs on the game thread, outside the game's own xLua state,\n"
        "-- so nothing here can corrupt combat scripts or the stage loader.\n"
        "--\n"
        "--   ss_ready()                     metadata graph built?\n"
        "--   ss_class(name)                 class handle, or nil\n"
        "--   ss_method(class, name[, argc]) method handle\n"
        "--   ss_args(method)                 { \"TypeName\", ... }\n"
        "--   ss_methods(class[, filter])    { \"Name/argc\", ... }\n"
        "--   ss_fields(class[, filter])     { \"Name\", ... }\n"
        "--   ss_field(class, name)          field handle\n"
        "--   ss_offset(field)               byte offset inside the object\n"
        "--   ss_invoke(method, obj, ...)    call it, unboxed result\n"
        "--   ss_read_i64/write_i64(obj, off, v)\n"
        "--   ss_read_i32/write_i32(obj, off, v)\n"
        "--   ss_class_of(obj)               class handle of a live object\n"
        "--   ss_log(text)                   write to the console tab\n"
        "\n"
        "ss_log(\"metadata ready = \" .. tostring(ss_ready()))\n"
        "\n"
        "-- Resolve a couple of engine types to prove the graph is live.\n"
        "-- ss_class returns nil for anything it cannot find, so a nil here\n"
        "-- means the name is wrong, not that the bridge is down.\n"
        "for _, name in { \"UnityEngine.Time\", \"UnityEngine.Vector3\" } do\n"
        "  local k = ss_class(name)\n"
        "  ss_log(string.format(\"%-24s %s\", name, tostring(k)))\n"
        "end\n";

    Card c;
    CardBegin(c, "Luau", obf::Decoded(kLuauLabel));

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.06f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.86f, 0.80f, 1.00f));
    const float h = fmaxf(S(150.0f), ImGui::GetContentRegionAvail().y - S(100.0f));
    ImGui::InputTextMultiline("##luausrc", src, sizeof(src), ImVec2(g_rowW, h));
    ImGui::PopStyleColor(2);

    ImGui::Dummy(ImVec2(0.0f, S(4.0f)));

    const float padW = c.w - kCardPad * 2.0f;
    const float bw   = (padW - S(12.0f)) * 0.5f;
    const ImVec2 row = ImGui::GetCursorScreenPos();
    if (CardButton("Run", bw)) LuauQueue(src);
    ImGui::SetCursorScreenPos(ImVec2(row.x + bw + S(12.0f), row.y));
    if (CardButton("Clear", bw)) src[0] = '\0';

    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(36.0f)));
    ImGui::TextDisabled("%s", LuauStatus());
    CardEnd(c);
}

void SafetyTab()
{
    Card c;

    CardBegin(c, "Anti-cheat", "state of this process");
    InfoRow("ACE on disk", AceInstalled() ? "yes" : "no",
            AceInstalled() ? kColMuted : kColFaint);
    InfoRow("ACE loaded", AceLoaded() ? "yes" : "no",
            AceLoaded() ? kColAccent : kColGreen);
    InfoRow("In module list", "no", kColGreen);
    InfoRow("Game code patched", "no", kColGreen);
    CardEnd(c);

    CardBegin(c, "Behaviour", "what the payload may touch");
    ToggleRow("Safe Mode", "Skips writes ACE is known to watch.", &g_cheat.safeMode);
    CardEnd(c);
}

void LogsTab()
{
    Card c;
    char desc[64];
    _snprintf_s(desc, _TRUNCATE, "last 200 lines  \xc2\xb7  %ld total", (long)g_logSeq);

    CardBegin(c, "Console", desc);
    const float h = fmaxf(S(180.0f), ImGui::GetContentRegionAvail().y - S(38.0f));
    ImGui::BeginChild("##logview", ImVec2(g_rowW, h), ImGuiChildFlags_Borders);
    {
        static char buf[12000];
        const int n = LogTail(buf, sizeof(buf), 200);
        if (n == 0)
            ImGui::TextDisabled("nothing logged yet \xe2\x80\x94 hook events appear here as the game runs");
        else
            ImGui::TextUnformatted(buf);

        static LONG lastSeq = -1;
        if (g_logSeq != lastSeq)
        {
            lastSeq = g_logSeq;
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, S(2.0f)));
    CardEnd(c);
}

void KeyCell(ImDrawList* dl, ImVec2 p, const char* key, const char* label)
{
    const ImVec2 ks = ImGui::CalcTextSize(key);
    const float cw = ks.x + S(12.0f);
    const float ch = S(20.0f);

    dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(255, 255, 255, 16), S(4.0f));
    dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(255, 255, 255, 26), S(4.0f));
    dl->AddText(ImVec2(p.x + S(6.0f), p.y + (ch - ks.y) * 0.5f), kColText, key);

    const float px = ImGui::GetFontSize();
    dl->AddText(ImVec2(p.x + cw + S(9.0f), p.y + (ch - px) * 0.5f), kColMuted, label);
}

void KeysCard(float width = 0.0f)
{
    static const char* const kKey[10] = { "INSERT", "F1", "F2", "F3", "F4",
                                          "F5", "F6", "F7", "F8", "F9" };
    static const char* const kAct[10] = { "Menu", "God", "Energy", "Damage", "Aura",
                                          "Reset", "Cooldown", "Dash", "Move", "Speed" };

    Card c;
    CardBegin(c, "Keys", "hotkeys while the menu is closed", width);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const int   cols = (g_rowW > S(300.0f)) ? 3 : 2;
    const int   rows = (10 + cols - 1) / cols;
    const float rowH = S(27.0f);
    const float colW = g_rowW / (float)cols;

    for (int i = 0; i < 10; ++i)
    {
        const ImVec2 cp(p.x + (i % cols) * colW, p.y + (i / cols) * rowH);
        KeyCell(dl, cp, kKey[i], kAct[i]);
    }

    ImGui::Dummy(ImVec2(g_rowW, rowH * (float)rows));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH * (float)rows));
    CardEnd(c);
}

void SettingsTab()
{
    Card c;

    const float contentW = ImGui::GetContentRegionAvail().x - S(6.0f);
    const float gap      = S(12.0f);
    const float cfgW     = (contentW - gap) * 0.40f;
    const float keysW    = contentW - gap - cfgW;

    CardBegin(c, "Appearance", "menu cosmetics");
    SliderRow("Wallpaper", "Opacity of the Phoenix image.", &g_wallAlpha, true, 0.0f, 255.0f, "%d");
    SliderRow("Backdrop", "Darkening applied over it.", &g_dimAlpha, true, 0.0f, 255.0f, "%d");
    SliderRow("UI Scale", "Scales the whole menu.", &g_fontScale, false, 0.8f, 1.6f, "%.2f");
    CardEnd(c);

    // Second row: two cards side by side, so the pane fits a 600px-tall window.
    const ImVec2 rowTop = ImGui::GetCursorScreenPos();

    // The path moved into the subtitle: as a caption it was only ballast, and
    // the space it held is what kept this tab scrolling.
    CardBegin(c, "Config", obf::Decoded(kCfgLabel), cfgW);
    {
        const float padW = c.w - kCardPad * 2.0f;
        const float bw   = (padW - S(12.0f)) * 0.5f;
        // Placed from an explicit row origin: SameLine's spacing was being
        // ignored here and the two buttons ended up different widths.
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (CardButton("Save", bw)) SaveConfig();
        ImGui::SetCursorScreenPos(ImVec2(row.x + bw + S(12.0f), row.y));
        if (CardButton("Load", bw)) LoadConfig();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(36.0f)));
        if (CardButton("Reset", padW))
        {
            g_wallAlpha = 150; g_dimAlpha = 145; g_fontScale = 1.0f;
            g_cheat.dmgMult = 1; g_cheat.defMult = 1;
            g_cheat.atkSpdMult = 1.0f; g_cheat.moveSpdMult = 1.0f; g_cheat.gameSpeed = 1.0f;
            g_cheat.auraDmg = 5000; g_cheat.auraLethal = true;
        }
    }
    CardEnd(c);

    ImGui::SetCursorScreenPos(ImVec2(rowTop.x + cfgW + gap, rowTop.y));
    KeysCard(keysW);
}

} // namespace

namespace menu {

int   g_selectedTab = 0;
int   g_shownTab    = 0;
float g_tabFade     = 1.0f;

// Lives outside Draw() so the overlay can ask whether the fade is still
// running before it skips the frame entirely.
static float g_openAnim = 0.0f;

bool Animating() { return g_openAnim > 0.001f; }

void ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    const ImVec4 accent(1.00f, 0.54f, 0.14f, 1.00f);

    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.GrabRounding      = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    style.WindowPadding     = ImVec2(12, 10);
    style.ScrollbarSize     = 8.0f;

    c[ImGuiCol_WindowBg]         = ImVec4(0.03f, 0.02f, 0.015f, 0.55f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Border]           = ImVec4(1.00f, 0.54f, 0.14f, 0.22f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.11f, 0.075f, 0.05f, 0.92f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.14f, 0.08f, 0.95f);
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = ImVec4(0.90f, 0.45f, 0.12f, 0.75f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button]           = ImVec4(0.13f, 0.09f, 0.06f, 0.92f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.62f, 0.30f, 0.08f, 0.90f);
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = ImVec4(0.48f, 0.20f, 0.04f, 0.38f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.78f, 0.38f, 0.09f, 0.55f);
    c[ImGuiCol_HeaderActive]     = accent;
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.78f, 0.38f, 0.09f, 0.70f);
    c[ImGuiCol_SeparatorActive]  = accent;
    c[ImGuiCol_ResizeGrip]       = ImVec4(1.00f, 0.54f, 0.14f, 0.15f);
    c[ImGuiCol_ResizeGripHovered]= ImVec4(1.00f, 0.54f, 0.14f, 0.60f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 0.54f, 0.14f, 0.95f);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(1.00f, 0.54f, 0.14f, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 0.54f, 0.14f, 0.60f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.00f, 0.54f, 0.14f, 0.85f);
    c[ImGuiCol_Text]             = ImVec4(0.98f, 0.94f, 0.89f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.62f, 0.57f, 0.52f, 1.00f);
}

void SetTab(int tab)
{
    if (tab < 0 || tab >= kTabCount) return;
    g_selectedTab = tab;
    g_shownTab    = tab;
    g_tabFade     = 1.0f;
}

void Draw(ImTextureID wallpaper, int wallpaperAlpha, int dimAlpha)
{
    {
        const float dtA = fminf(0.05f, ImGui::GetIO().DeltaTime);
        const float target = g_menuOpen ? 1.0f : 0.0f;
        g_openAnim += (target - g_openAnim) * fminf(1.0f, dtA * 14.0f);
        if (!g_menuOpen && g_openAnim < 0.01f) return;
    }

    static bool themed = false;
    if (!themed) { ApplyTheme(); themed = true; }

    g_uiScale = fmaxf(0.8f, fminf(1.6f, g_fontScale));

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 disp = io.DisplaySize;
    if (disp.x < 320.0f || disp.y < 240.0f)
        disp = ImVec2(1280.0f, 720.0f);

    const float minW = 800.0f, minH = 520.0f;
    const float maxW = fmaxf(minW, disp.x - 16.0f);
    const float maxH = fmaxf(minH, disp.y - 16.0f);

    // Size is owned by the corner grip and persisted in the config.
    float w = (g_cfgW > 0.0f) ? g_cfgW : 900.0f;
    float h = (g_cfgH > 0.0f) ? g_cfgH : 600.0f;
    w = fminf(fmaxf(w, minW), maxW);
    h = fminf(fmaxf(h, minH), maxH);

    // Position is owned by the header drag. A stale saved position from another
    // resolution is clamped instead of being allowed off-screen.
    float px = (g_cfgX >= 0.0f) ? g_cfgX : 48.0f;
    float py = (g_cfgY >= 0.0f) ? g_cfgY : 32.0f;
    px = fmaxf(0.0f, fminf(px, disp.x - w));
    py = fmaxf(0.0f, fminf(py, disp.y - h));

    // Keep the window fully inside the screen even after a resize at an edge.
    w = fminf(w, disp.x - px);
    h = fminf(h, disp.y - py);

    // The config values are the source of truth: normalise them to what is
    // actually on screen, so the drag and resize deltas applied later this frame
    // build on the clamped state instead of being overwritten at the end.
    g_cfgW = w;
    g_cfgH = h;
    g_cfgX = px;
    g_cfgY = py;

    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);

    const bool open = ImGui::Begin(MenuWindowId(), nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings);
    if (!open) { ImGui::End(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float  right = pos.x + size.x;
    const float  bottom = pos.y + size.y;

    // ---- backdrop ---------------------------------------------------------
    // Opaque base first: the menu is self-contained, the game never bleeds
    // through no matter how far the backdrop slider is pulled down.
    dl->AddRectFilled(pos, ImVec2(right, bottom), IM_COL32(9, 7, 6, 255), 10.0f);

    if (wallpaper)
    {
        // Cover-fit: crop the texture instead of stretching it, so the art
        // keeps its aspect ratio at every window size.
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        const float winAspect = size.x / fmaxf(1.0f, size.y);
        const float imgAspect = (g_wallAspect > 0.01f) ? g_wallAspect : winAspect;
        if (imgAspect > winAspect)
        {
            const float vis = winAspect / imgAspect;
            u0 = (1.0f - vis) * 0.5f;
            u1 = 1.0f - u0;
        }
        else
        {
            const float vis = imgAspect / winAspect;
            v0 = (1.0f - vis) * 0.5f;
            v1 = 1.0f - v0;
        }
        dl->AddImageRounded(wallpaper, pos, ImVec2(right, bottom),
                            ImVec2(u0, v0), ImVec2(u1, v1),
                            IM_COL32(255, 255, 255, (int)(wallpaperAlpha * g_openAnim)),
                            10.0f);
    }

    {
        // The slider biases the art, it does not control legibility: even at 0
        // there is a floor, so the panel never turns into raw wallpaper.
        const int scrim = (int)fminf(255.0f, 55.0f + dimAlpha * 0.78f);
        dl->AddRectFilled(pos, ImVec2(right, bottom),
                          IM_COL32(0, 0, 0, (int)(scrim * g_openAnim)), 10.0f);
    }
    dl->AddRect(pos, ImVec2(right, bottom), IM_COL32(255, 138, 36, 70), 10.0f, 0, 1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_openAnim);

    const float bodyTop = pos.y + kHeaderH;
    const float bodyBot = pos.y + size.y - kFooterH - kActionH;

    // ---- panels -----------------------------------------------------------
    // Drawn before any text: the bands must sit under the header, not over it.
    // The two outer bands carry the window's rounding themselves. Drawn square
    // they cover the rounded base and the panel reads as a hard-cornered box.
    dl->AddRectFilled(pos, ImVec2(right, bodyTop), IM_COL32(9, 7, 6, 186),
                      10.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(pos.x, bodyTop), ImVec2(right, pos.y + size.y),
                      IM_COL32(9, 7, 6, 222), 10.0f, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(ImVec2(pos.x, bodyTop), ImVec2(pos.x + kRailW, bodyBot),
                      IM_COL32(9, 7, 6, 244));
    dl->AddLine(ImVec2(pos.x + kRailW, bodyTop), ImVec2(pos.x + kRailW, bodyBot),
                IM_COL32(255, 138, 36, 45), 1.0f);

    // ---- header -----------------------------------------------------------
    {
        // Drag handle across the title band. The close button sits outside it.
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 6.0f, pos.y + 4.0f));
        ImGui::InvisibleButton("##drag", ImVec2(size.x - 70.0f, kHeaderH - 8.0f));
        const bool dragHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            g_cfgX += d.x;
            g_cfgY += d.y;
        }
        if (dragHot && ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        ImFont* fb = BoldFont();
        const float tpx = S(28.0f);
        dl->AddText(fb, tpx, ImVec2(pos.x + 24.0f, pos.y + S(15.0f)), kColTitle, "STELLA SORA");
        dl->AddText(ImGui::GetFont(), S(14.0f), ImVec2(pos.x + 26.0f, pos.y + S(48.0f)),
                    kColMuted, "by Azels56   \xc2\xb7   v2.0");

        // close
        const ImVec2 cp(right - 44.0f, pos.y + 24.0f);
        ImGui::SetCursorScreenPos(cp);
        ImGui::InvisibleButton("##close", ImVec2(S(26.0f), S(26.0f)));
        const bool chov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) g_menuOpen = false;
        if (chov)
            dl->AddRectFilled(cp, ImVec2(cp.x + S(26.0f), cp.y + S(26.0f)),
                              IM_COL32(255, 138, 36, 40), S(6.0f));
        const ImU32 xc = chov ? kColAccent : IM_COL32(190, 180, 170, 255);
        const float e = S(7.0f);
        const ImVec2 cm(cp.x + S(13.0f), cp.y + S(13.0f));
        dl->AddLine(ImVec2(cm.x - e, cm.y - e), ImVec2(cm.x + e, cm.y + e), xc, 1.6f);
        dl->AddLine(ImVec2(cm.x - e, cm.y + e), ImVec2(cm.x + e, cm.y - e), xc, 1.6f);
    }

    dl->AddLine(ImVec2(pos.x, pos.y + kHeaderH), ImVec2(right, pos.y + kHeaderH),
                IM_COL32(255, 138, 36, 45), 1.0f);

    // ---- rail -------------------------------------------------------------
    // Seven tabs no longer fit at the minimum window height, so the pitch is
    // squeezed before the rail is allowed to run into the action bar.
    const float railTop = bodyTop + S(16.0f);
    const float railBot = bodyBot - S(8.0f);
    float step = kTabH + kTabGap;
    if (railTop + step * (float)kTabCount > railBot)
        step = fmaxf(S(34.0f), (railBot - railTop) / (float)kTabCount);
    const float tabH = fminf(kTabH, step - S(2.0f));

    for (int i = 0; i < kTabCount; ++i)
    {
        const ImVec2 tp(pos.x + kTabX, railTop + i * step);
        if (DrawTab(dl, tp, i, g_selectedTab == i, tabH))
            g_selectedTab = i;
    }

    // ---- content ----------------------------------------------------------
    const float cw = size.x - kRailW - 32.0f;
    const float ch = size.y - kHeaderH - kActionH - kFooterH - 26.0f;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + kRailW + 16.0f, bodyTop + S(16.0f)));
    ImGui::BeginChild("##content", ImVec2(cw, ch), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    g_origin = ImGui::GetCursorScreenPos();

    {
        const float dt = fmaxf(0.001f, ImGui::GetIO().DeltaTime) * 10.0f;
        if (g_shownTab == g_selectedTab) g_tabFade = fminf(1.0f, g_tabFade + dt);
        else { g_tabFade -= dt; if (g_tabFade <= 0.0f) { g_tabFade = 0.0f; g_shownTab = g_selectedTab; } }

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_openAnim * g_tabFade);
        switch (g_shownTab)
        {
            case 0: CombatTab();   break;
            case 1: GameTab();     break;
            case 2: SkillsTab();   break;
            case 3: StatusTab();   break;
            case 4: ScriptTab();   break;
            case 5: SafetyTab();   break;
            case 6: LogsTab();     break;
            case 7: SettingsTab(); break;
            default: break;
        }
        ImGui::PopStyleVar();
    }
    const float scrollMax = ImGui::GetScrollMaxY();
    ImGui::EndChild();

    // Fade the bottom edge when the pane scrolls, so a half-visible row reads as
    // "there is more below" instead of a clipped card.
    if (scrollMax > 1.0f)
    {
        const float cx0 = pos.x + kRailW + 8.0f;
        const float cx1 = right - 10.0f;
        const float cy1 = bodyTop + S(16.0f) + ch + 10.0f;
        dl->AddRectFilledMultiColor(ImVec2(cx0, cy1 - S(42.0f)), ImVec2(cx1, cy1),
                                    IM_COL32(9, 7, 6, 0), IM_COL32(9, 7, 6, 0),
                                    IM_COL32(9, 7, 6, 250), IM_COL32(9, 7, 6, 250));
    }

    // ---- action bar -------------------------------------------------------
    {
        const float ay = bodyBot;
        dl->AddLine(ImVec2(pos.x, ay), ImVec2(right, ay), IM_COL32(255, 138, 36, 35), 1.0f);

        const char* labels[3] = { "Reset Skills", "Reset Stats", "Kill Enemies" };
        float bx = pos.x + kRailW + 16.0f;
        for (int i = 0; i < 3; ++i)
        {
            ImGui::SetCursorScreenPos(ImVec2(bx, ay + S(9.0f)));
            ImGui::PushID(i);
            const bool hit = CardButton(labels[i], S(132.0f));
            ImGui::PopID();
            if (hit)
            {
                // Recorded only. features.cpp drains this on the game thread;
                // the action bar itself runs inside the Present hook.
                if (i == 0) QueueAction(ACT_RESET_SKILLS);
                if (i == 1) QueueAction(ACT_RESET_STATS);
                if (i == 2) QueueAction(ACT_KILL_MOBS);
            }
            bx += S(132.0f) + S(12.0f);
        }
    }

    // ---- footer -----------------------------------------------------------
    {
        const float fy = pos.y + size.y - kFooterH;
        dl->AddLine(ImVec2(pos.x, fy), ImVec2(right, fy), IM_COL32(255, 138, 36, 35), 1.0f);

        const float ty = fy + (kFooterH - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(pos.x + 24.0f, ty), kColMuted, "INSERT toggles the menu");

        const bool ace = AceLoaded();
        char aceBuf[64];
        _snprintf_s(aceBuf, _TRUNCATE, "ACE %s", ace ? "loaded" : "absent");

        char fps[64];
        _snprintf_s(fps, _TRUNCATE, "%.0f fps", ImGui::GetIO().Framerate);

        const ImVec2 fsz = ImGui::CalcTextSize(fps);
        const ImVec2 asz = ImGui::CalcTextSize(aceBuf);
        const ImVec2 osz = ImGui::CalcTextSize("Overlay live");

        // The two pips sat to the left of their labels with no pip on the fps
        // reading, so the footer read as a three-item bullet list with one
        // bullet missing. Same separator as the card subtitles, and the state
        // word itself carries the colour.
        const char*  sep  = "  \xc2\xb7  ";
        const float  sepW = ImGui::CalcTextSize(sep).x;

        float x = right - 40.0f - fsz.x;
        dl->AddText(ImVec2(x, ty), kColFaint, fps);

        x -= sepW;
        dl->AddText(ImVec2(x, ty), kColFaint, sep);

        x -= osz.x;
        dl->AddText(ImVec2(x, ty), kColMuted, "Overlay live");

        x -= sepW;
        dl->AddText(ImVec2(x, ty), kColFaint, sep);

        x -= asz.x;
        dl->AddText(ImVec2(x, ty), ace ? kColAccent : kColGreen, aceBuf);
    }

    // ---- resize grip ------------------------------------------------------
    {
        const ImVec2 gp(right - S(22.0f), bottom - S(22.0f));
        ImGui::SetCursorScreenPos(gp);
        ImGui::InvisibleButton("##grip", ImVec2(S(20.0f), S(20.0f)));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            g_cfgW = fminf(fmaxf(g_cfgW + d.x, minW), maxW);
            g_cfgH = fminf(fmaxf(g_cfgH + d.y, minH), maxH);
        }
        const ImU32 gc = hot ? kColAccent : IM_COL32(150, 138, 126, 190);
        for (int i = 0; i < 3; ++i)
        {
            const float o = S(6.0f) + (float)i * S(4.5f);
            dl->AddLine(ImVec2(gp.x + o, gp.y + S(16.0f)),
                        ImVec2(gp.x + S(16.0f), gp.y + o), gc, 1.5f);
        }
    }

    ImGui::PopStyleVar();

    ImGui::End();
}

} // namespace menu
