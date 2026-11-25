#include "app/home_menu.h"
#include "app/online.h"
#include "app/neon_ui.h"
#include "app/online_overlay.h"
#include "app_version.h" // generated: cmake/app_version.cmake
#include "app/i18n.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace app {

using Course = rt::RaceCourse;

namespace {

using neon::fill;
using neon::fit;
using neon::gear;
using neon::glow;
using neon::italic;
using neon::measure;
using neon::plus;
using neon::rgba;
using neon::rounded;

ImFont *load_font(ImFontAtlas *atlas, std::initializer_list<std::string> paths) {
    for (const std::string &path : paths)
        if (std::ifstream(path, std::ios::binary))
            if (auto *font = atlas->AddFontFromFileTTF(path.c_str())) return font;
    return nullptr;
}

// The home screen's palette: night-blue panels, white type, neon blue for
// what is chosen and the Online mode, a yellow-orange call to action, a red
// accent.
ImU32 rgb24(uint32_t c, float a = 1) { return rgba(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }

// Tags lean forward like the type: a parallelogram whose top edge is pushed
// right by `lean` of its height.
constexpr float lean = 0.24f;

void slab(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
    const float k = (b.y - a.y) * lean;
    const ImVec2 p[] = {{a.x + k, a.y}, {b.x, a.y}, {b.x - k, b.y}, {a.x, b.y}};
    const int first = draw->VtxBuffer.Size;
    draw->AddConvexPolyFilled(p, 4, top);
    if (top != bottom)
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, a, {a.x, b.y}, top, bottom);
}

// A neon line (the screen's edges).
void streak(ImDrawList *draw, ImVec2 a, ImVec2 b, ImVec4 c, float strength, float scale) {
    for (int i = 4; i >= 1; --i)
        draw->AddLine(a, b, ImGui::GetColorU32({c.x, c.y, c.z, strength * 0.12f * (1.f - (i - 1) / 4.f)}), (2 + i * 4.f) * scale);
    draw->AddLine(a, b, ImGui::GetColorU32({std::min(1.f, c.x + 0.4f), std::min(1.f, c.y + 0.3f), 1, strength}), 1.6f * scale);
}

// Icons, drawn: each in an s x s box around c.
void person(ImDrawList *d, ImVec2 c, float s, ImU32 col) {
    d->AddCircleFilled({c.x, c.y - s * 0.17f}, s * 0.21f, col, 20);
    d->PathArcTo({c.x, c.y + s * 0.48f}, s * 0.36f, IM_PI, 2 * IM_PI, 16);
    d->PathFillConvex(col);
}
void wifi(ImDrawList *d, ImVec2 c, float s, ImU32 col) {
    const ImVec2 o = {c.x, c.y + s * 0.32f};
    d->AddCircleFilled(o, s * 0.09f, col, 12);
    for (float r : {0.30f, 0.52f}) {
        d->PathArcTo(o, s * r, -IM_PI * 0.78f, -IM_PI * 0.22f, 14);
        d->PathStroke(col, ImDrawFlags_None, s * 0.11f);
    }
}
// The Solo card's emblem: a speedometer, its needle near the top.
void speedometer(ImDrawList *d, ImVec2 c, float r, float scale, float time) {
    const float start = IM_PI * 0.75f, end = IM_PI * 2.25f;
    d->AddCircleFilled(c, r, rgba(0.02f, 0.06f, 0.15f, 0.88f), 48);
    for (int i = 3; i >= 1; --i) d->AddCircle(c, r + i * 1.5f * scale, rgba(0.35f, 0.70f, 1, 0.10f), 48, 2.5f * scale);
    d->AddCircle(c, r, rgba(0.55f, 0.80f, 1, 0.75f), 48, 1.8f * scale);
    // the scale's band: blue into yellow into red
    const int n = 36;
    for (int i = 0; i < n; ++i) {
        const float t0 = float(i) / n, t1 = float(i + 1) / n, a0 = start + (end - start) * t0, a1 = start + (end - start) * t1;
        const float m = (t0 + t1) * 0.5f;
        const ImVec4 col = m < 0.6f ? ImVec4(0.30f + m * 1.1f, 0.75f + m * 0.2f, 1 - m * 1.2f, 0.9f)
                                    : ImVec4(1, 0.88f - (m - 0.6f) * 1.8f, 0.25f - (m - 0.6f) * 0.4f, 0.95f);
        d->PathArcTo(c, r * 0.84f, a0, a1, 3);
        d->PathStroke(ImGui::GetColorU32(col), ImDrawFlags_None, 4.5f * scale);
    }
    for (int i = 0; i <= 16; ++i) { // ticks, long every other
        const float a = start + (end - start) * i / 16.f, in = i % 2 ? 0.66f : 0.58f;
        d->AddLine({c.x + std::cos(a) * r * in, c.y + std::sin(a) * r * in}, {c.x + std::cos(a) * r * 0.72f, c.y + std::sin(a) * r * 0.72f},
                   rgba(0.9f, 0.95f, 1, i % 2 ? 0.45f : 0.85f), (i % 2 ? 1.2f : 2.f) * scale);
    }
    const float needle = start + (end - start) * (0.80f + 0.04f * std::sin(time * 1.7f));
    const ImVec2 tip = {c.x + std::cos(needle) * r * 0.80f, c.y + std::sin(needle) * r * 0.80f};
    const ImVec2 side = {-std::sin(needle) * 3.2f * scale, std::cos(needle) * 3.2f * scale};
    d->AddLine(c, tip, rgba(1, 0.45f, 0.25f, 0.30f), 7 * scale);
    d->AddTriangleFilled({c.x + side.x, c.y + side.y}, {c.x - side.x, c.y - side.y}, tip, rgba(1, 0.42f, 0.22f));
    d->AddCircleFilled(c, 6 * scale, rgba(0.92f, 0.96f, 1), 20);
    d->AddCircleFilled(c, 2.6f * scale, rgba(0.05f, 0.08f, 0.18f), 12);
}

// The Online card's emblem: a turning globe of lines, players linked across it.
void globe(ImDrawList *d, ImVec2 c, float r, float scale, float time) {
    d->AddCircleFilled(c, r, rgba(0.03f, 0.11f, 0.28f, 0.88f), 48);
    for (int i = 3; i >= 1; --i) d->AddCircle(c, r + i * 1.5f * scale, rgba(0.35f, 0.70f, 1, 0.12f), 48, 2.5f * scale);
    const ImU32 line = rgba(0.45f, 0.78f, 1, 0.42f);
    for (float lat : {-0.87f, -0.45f, 0.f, 0.45f, 0.87f}) { // parallels
        const float y = c.y + r * std::sin(lat), half = r * std::cos(lat);
        d->AddLine({c.x - half, y}, {c.x + half, y}, line, 1.2f * scale);
    }
    const float turn = std::fmod(time * 0.25f, IM_PI / 4);
    for (int i = 0; i < 4; ++i) { // meridians, turning
        const float a = turn + i * IM_PI / 4, rx = r * std::fabs(std::cos(a));
        if (rx > 1) d->AddEllipse(c, {rx, r}, line, 0, 40, 1.2f * scale);
    }
    d->AddCircle(c, r, rgba(0.55f, 0.85f, 1, 0.85f), 48, 1.8f * scale);
    // three players, linked by arcs a dot runs along
    const ImVec2 at[] = {{c.x - r * 0.58f, c.y - r * 0.12f}, {c.x + r * 0.46f, c.y - r * 0.46f}, {c.x + r * 0.22f, c.y + r * 0.52f}};
    const ImU32 arc = rgba(1, 0.80f, 0.30f, 0.9f);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a = at[i], b = at[(i + 1) % 3];
        const ImVec2 mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const ImVec2 ctl = {c.x + (mid.x - c.x) * 1.9f, c.y + (mid.y - c.y) * 1.9f};
        d->PathLineTo(a);
        d->PathBezierQuadraticCurveTo(ctl, b, 16);
        d->PathStroke(arc, ImDrawFlags_None, 1.8f * scale);
        const float t = std::fmod(time * 0.5f + i / 3.f, 1.f), u = 1 - t;
        const ImVec2 dot = {u * u * a.x + 2 * u * t * ctl.x + t * t * b.x, u * u * a.y + 2 * u * t * ctl.y + t * t * b.y};
        d->AddCircleFilled(dot, 2.6f * scale, rgba(1, 0.95f, 0.75f), 10);
    }
    for (const ImVec2 &p : at) {
        d->AddCircleFilled(p, 6.5f * scale, rgba(0.30f, 1, 0.55f, 0.22f), 14);
        d->AddCircleFilled(p, 3.6f * scale, rgba(0.35f, 1, 0.60f), 12);
    }
}

void gamepad(ImDrawList *d, ImVec2 c, float s, ImU32 col, ImU32 ink) {
    d->AddRectFilled({c.x - s * 0.5f, c.y - s * 0.28f}, {c.x + s * 0.5f, c.y + s * 0.28f}, col, s * 0.22f);
    d->AddLine({c.x - s * 0.3f, c.y}, {c.x - s * 0.1f, c.y}, ink, s * 0.08f);
    d->AddLine({c.x - s * 0.2f, c.y - s * 0.1f}, {c.x - s * 0.2f, c.y + s * 0.1f}, ink, s * 0.08f);
    d->AddCircleFilled({c.x + s * 0.18f, c.y - s * 0.05f}, s * 0.055f, ink);
    d->AddCircleFilled({c.x + s * 0.3f, c.y + s * 0.07f}, s * 0.055f, ink);
}
void shifter(ImDrawList *d, ImVec2 c, float s, ImU32 col) {
    const float t = s * 0.11f, y0 = c.y - s * 0.38f, y1 = c.y + s * 0.38f;
    for (float x : {-0.34f, 0.f, 0.34f}) d->AddLine({c.x + x * s, y0}, {c.x + x * s, x > 0 ? c.y : y1}, col, t);
    d->AddLine({c.x - 0.34f * s, c.y}, {c.x + 0.34f * s, c.y}, col, t);
    d->AddCircleFilled({c.x + 0.34f * s, c.y + 0.3f * s}, s * 0.12f, col);
}
void bars(ImDrawList *d, ImVec2 c, float s, ImU32 col) {
    for (int i = 0; i < 3; ++i) {
        const float x = c.x - s * 0.42f + i * s * 0.32f, h = s * (0.35f + i * 0.3f);
        d->AddRectFilled({x, c.y + s * 0.4f - h}, {x + s * 0.2f, c.y + s * 0.4f}, col);
    }
}
void exit_icon(ImDrawList *d, ImVec2 c, float s, ImU32 col, float t) {
    const ImVec2 p[] = {{c.x + s * 0.05f, c.y - s * 0.4f}, {c.x - s * 0.4f, c.y - s * 0.4f}, {c.x - s * 0.4f, c.y + s * 0.4f},
                        {c.x + s * 0.05f, c.y + s * 0.4f}};
    d->AddPolyline(p, 4, col, ImDrawFlags_None, t);
    d->AddLine({c.x - s * 0.12f, c.y}, {c.x + s * 0.42f, c.y}, col, t);
    d->AddLine({c.x + s * 0.24f, c.y - s * 0.18f}, {c.x + s * 0.43f, c.y}, col, t);
    d->AddLine({c.x + s * 0.24f, c.y + s * 0.18f}, {c.x + s * 0.43f, c.y}, col, t);
}
void monitor(ImDrawList *d, ImVec2 c, float s, ImU32 col, float t) {
    d->AddRect({c.x - s * 0.45f, c.y - s * 0.35f}, {c.x + s * 0.45f, c.y + s * 0.2f}, col, s * 0.05f, 0, t);
    d->AddLine({c.x, c.y + s * 0.2f}, {c.x, c.y + s * 0.38f}, col, t);
    d->AddLine({c.x - s * 0.22f, c.y + s * 0.4f}, {c.x + s * 0.22f, c.y + s * 0.4f}, col, t);
}
void chevron_down(ImDrawList *d, ImVec2 c, float s, ImU32 col, float t) {
    const ImVec2 p[] = {{c.x - s * 0.3f, c.y - s * 0.12f}, {c.x, c.y + s * 0.16f}, {c.x + s * 0.3f, c.y - s * 0.12f}};
    d->AddPolyline(p, 3, col, ImDrawFlags_None, t);
}
void crown(ImDrawList *d, ImVec2 c, float s, ImU32 col) {
    const ImVec2 p[] = {{c.x - s * 0.45f, c.y + s * 0.3f}, {c.x - s * 0.45f, c.y - s * 0.25f}, {c.x - s * 0.18f, c.y + s * 0.02f},
                        {c.x, c.y - s * 0.35f}, {c.x + s * 0.18f, c.y + s * 0.02f}, {c.x + s * 0.45f, c.y - s * 0.25f},
                        {c.x + s * 0.45f, c.y + s * 0.3f}};
    d->AddConcavePolyFilled(p, 7, col);
}

// A long invite code, shortened to fit: its start, an ellipsis, its end.
std::string shorten(ImFont *font, float size, const std::string &text, float width) {
    if (measure(font, size, text.c_str()).x <= width || text.size() < 16) return text;
    // whole groups: "DTN1-ABCDE-FGHIJ...XYZ12"
    const auto dash = text.rfind('-');
    const size_t cut = dash == std::string::npos ? text.size() - 5 : dash;
    const std::string tail = "..." + text.substr(cut + (dash == std::string::npos ? 0 : 1));
    std::string head = text.substr(0, cut);
    while (head.size() > 6 && measure(font, size, (head + tail).c_str()).x > width) {
        const auto d = head.rfind('-');
        head.resize(d != std::string::npos && d > 4 ? d : head.size() - 1);
    }
    return head + tail;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

} // namespace

void HomeMenu::init() {
    auto *atlas = ImGui::GetIO().Fonts;
    auto *fallback = atlas->AddFontDefaultVector();
    i18n::add_fonts(); // Chinese, Japanese, Arabic glyphs merged into it (the launcher's font)
    display_ = body_ = fallback;
#ifdef _WIN32
    if (const char *windows = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "WINDIR")) {
        const std::string folder = std::string(windows) + "/Fonts/";
        if (auto *font = load_font(atlas, {folder + "bahnschrift.ttf"})) display_ = font;
        if (auto *font = load_font(atlas, {folder + "segoeuib.ttf"})) {
            body_ = font;
            i18n::add_fonts(); // and into the menu's text
        }
    }
#elif defined(__linux__)
    // The desktop's own bold sans when there is one; the default font otherwise.
    const std::initializer_list<std::string> bold = {
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"};
    if (auto *font = load_font(atlas, bold)) display_ = body_ = font;
#endif
}

void HomeMenu::paste_invite(const std::string &code) {
    std::snprintf(invite_, sizeof invite_, "%s", code.c_str());
    invite_text_ = code;
    pasted_ = true;
    seen_clip_ = code;
}

HomeMenu::Action HomeMenu::draw(bool has_race, bool loading, const char *message, float opacity, const HomeOnline &online) {
    const auto *view = ImGui::GetMainViewport();
    const float w = view->Size.x, h = view->Size.y;
    const float scale = std::max(0.13f, std::min(h / 900.f, w / 980.f));
    const float left = std::max(w * 0.052f, 31.f * scale);
    // Centred, but high enough that the tallest case (a two-line message under
    // the buttons) clears the bottom bar, so the column never jumps.
    const float top = std::max(26.f * scale, std::min(h * 0.5f - 380.f * scale, h - 760.f * scale));
    const float panel = 560.f * scale;
    const float time = float(ImGui::GetTime());
    const auto px = [&](float x, float y) { return ImVec2(view->Pos.x + x, view->Pos.y + y); };
    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Daytona home", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    auto *draw = ImGui::GetWindowDrawList();
    const ImU32 white = rgba(0.97f, 0.98f, 1), muted = rgba(0.70f, 0.78f, 0.90f), ink = rgba(0.06f, 0.05f, 0.03f);
    const ImU32 red = rgba(0.93f, 0.22f, 0.20f), yellow = rgba(1, 0.83f, 0.20f), orange = rgba(1, 0.52f, 0.13f);
    const ImU32 sky = rgba(0.22f, 0.58f, 1), cyan = rgba(0.38f, 0.80f, 1), green = rgba(0.25f, 0.88f, 0.48f);
    const ImU32 amber = rgba(1, 0.80f, 0.28f), alarm = rgba(1, 0.42f, 0.38f), navy = rgba(0.03f, 0.06f, 0.13f);
    const ImU32 focus_col = rgba(1, 0.9f, 0.35f);
    const ImVec4 neon_blue(0.25f, 0.70f, 1, 1), neon_red(1, 0.22f, 0.25f, 1);
    auto nav_focus = [] { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; };
    struct Hit {
        bool clicked, focused, hovered, held;
    };
    auto hit = [&](const char *id, ImVec2 a, ImVec2 b) {
        ImGui::SetCursorScreenPos(a);
        const bool clicked = ImGui::InvisibleButton(id, {std::max(1.f, b.x - a.x), std::max(1.f, b.y - a.y)},
                                                    ImGuiButtonFlags_EnableNav);
        return Hit{clicked, nav_focus(), ImGui::IsItemHovered(), ImGui::IsItemActive()};
    };
    auto focus_ring = [&](const Hit &h, const std::vector<ImVec2> &shape) {
        if (h.focused) draw->AddPolyline(shape.data(), int(shape.size()), focus_col, ImDrawFlags_Closed, 2.5f * scale);
        else if (h.hovered) draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 1, 1, 0.5f), ImDrawFlags_Closed, 1.5f * scale);
    };

    const bool in_room = online.room == HomeOnline::Room::In, connecting = online.room == HomeOnline::Room::Connecting;
    if (loading) tab_ = Tab::Solo;
    const bool solo_tab = tab_ == Tab::Solo;

    // The panel's shade over the scene, and neon on the screen's edges.
    draw->AddRectFilledMultiColor(px(0, 0), px(left + panel + 220 * scale, h), rgba(0.02f, 0.05f, 0.13f, 0.78f),
                                  rgba(0.02f, 0.05f, 0.13f, 0), rgba(0.02f, 0.05f, 0.13f, 0), rgba(0.02f, 0.05f, 0.13f, 0.78f));
    for (int i = 0; i < 3; ++i) {
        const float x = i * 10.f * scale;
        streak(draw, px(x - 30 * scale, h * (0.98f - i * 0.04f)), px(x + 70 * scale + i * 26 * scale, h * (0.30f - i * 0.12f)),
               neon_blue, 0.55f - i * 0.15f, scale);
    }
    streak(draw, px(w - 150 * scale, 0), px(w, 120 * scale), neon_red, 0.35f, scale);
    streak(draw, px(w - 70 * scale, h), px(w + 10 * scale, h - 170 * scale), neon_red, 0.5f, scale);

    // Tag line with a red-white-blue chevron run.
    {
        const ImU32 chevrons[] = {red, white, sky};
        for (int i = 0; i < 3; ++i) {
            const float x = left + i * 11 * scale;
            slab(draw, px(x, top - 1 * scale), px(x + 8 * scale, top + 12 * scale), chevrons[i], chevrons[i]);
        }
        draw->AddText(body_, 14 * scale, px(left + 42 * scale, top - 3 * scale), muted, TX("M O D E L   2    /    P C   E D I T I O N"));
    }
    // Title: italic, white into gold, over a hard shadow; a checkered flag
    // waving off its end.
    {
        const float size = 92 * scale;
        const char *title = "DAYTONA";
        const ImVec2 at = px(left - 3 * scale, top + 16 * scale);
        const ImVec2 wide = measure(display_, size, title);
        const float cell = 11 * scale;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) {
                if ((i + j) % 2) continue;
                const float wave = std::sin(time * 2.2f + i * 0.7f) * 3 * scale;
                const float x = at.x + wide.x + 4 * scale + i * cell + j * cell * 0.25f, y = at.y + 12 * scale + j * cell + wave - i * 2 * scale;
                const float fade = 0.85f * (1 - i / 6.f);
                draw->AddQuadFilled({x, y}, {x + cell, y - cell * 0.12f}, {x + cell * 1.25f, y + cell * 0.9f},
                                    {x + cell * 0.25f, y + cell}, rgba(0.92f, 0.94f, 1, fade));
            }
        italic(draw, display_, size, {at.x + 5 * scale, at.y + 5 * scale}, rgba(0.02f, 0.04f, 0.10f, 0.75f), title);
        italic(draw, display_, size, at, white, title, 0.2f, rgba(1, 0.80f, 0.32f));
    }
    // "ARCADE NATIVE" on a red tag.
    {
        const char *tag = "A R C A D E   N A T I V E";
        const float size = 15 * scale;
        const ImVec2 wide = measure(body_, size, tag);
        const ImVec2 a = px(left, top + 118 * scale);
        const ImVec2 b = {a.x + wide.x + 46 * scale, a.y + 27 * scale};
        slab(draw, a, b, rgba(0.95f, 0.25f, 0.22f), rgba(0.78f, 0.12f, 0.14f));
        for (int i = 0; i < 3; ++i)
            slab(draw, {b.x + (4 + i * 11) * scale, a.y}, {b.x + (11 + i * 11) * scale, b.y}, yellow, orange);
        italic(draw, body_, size, {a.x + 22 * scale, a.y + (27 * scale - size) * 0.5f}, white, tag, 0.12f);
    }

    Action action = Stay;
    if (pending_ != Stay && !online.name.empty()) {
        action = pending_;
        pending_ = Stay;
    }

    // The Online state, in words (its card, the pill).
    const char *status_word;
    ImU32 status_col;
    if (in_room) status_word = TX("In a room"), status_col = green;
    else if (connecting) status_word = TX("Connecting..."), status_col = amber;
    else if (online.server.empty()) status_word = TX("No server"), status_col = muted;
    else
        switch (online.probe) {
        case HomeOnline::Probe::Ok: status_word = TX("Online"), status_col = green; break;
        case HomeOnline::Probe::Offline: status_word = TX("Offline"), status_col = alarm; break;
        case HomeOnline::Probe::Version: status_word = TX("Other version"), status_col = alarm; break;
        default: status_word = TX("Testing..."), status_col = amber; break;
        }

    // The two mode cards, the same size: tabs. Online glows a little even
    // when not chosen (the emphasis).
    const float card_y = top + 160 * scale, card_h = 158 * scale, gap = 14 * scale;
    const float card_w = (panel - gap) * 0.5f;
    {
        const float k = 10 * scale, r = 12 * scale;
        // the frame both share: a gradient, two light streaks, a badge for the icon, a chip at the foot
        auto card = [&](const char *id, float x, bool on, bool emphasis, const char *title, const char *line,
                        const char *chip, ImU32 chip_col, bool chip_dot) {
            const ImVec2 a = px(x, card_y), b = px(x + card_w, card_y + card_h);
            const Hit h = hit(id, a, b);
            const auto shape = rounded(a, b, k, r);
            fill(draw, shape, on ? rgba(0.11f, 0.30f, 0.62f, 0.96f) : rgba(0.06f, 0.13f, 0.28f, 0.93f),
                 on ? rgba(0.04f, 0.12f, 0.32f, 0.96f) : rgba(0.02f, 0.05f, 0.13f, 0.93f), a.y, b.y);
            ImGui::PushClipRect(a, b, true);
            for (int i = 0; i < 2; ++i) { // speed streaks across the card
                const float x0 = a.x + card_w * (0.52f + i * 0.2f), wdt = (i ? 18.f : 46.f) * scale;
                draw->AddQuadFilled({x0 + 60 * scale, a.y}, {x0 + 60 * scale + wdt, a.y}, {x0 - 40 * scale + wdt, b.y},
                                    {x0 - 40 * scale, b.y}, rgba(0.7f, 0.85f, 1, on ? 0.07f : 0.04f));
            }
            ImGui::PopClipRect();
            if (on) glow(draw, shape, neon_blue, 1.f, scale);
            else if (emphasis) glow(draw, shape, neon_blue, 0.42f + 0.12f * std::sin(time * 2.f), scale);
            else draw->AddPolyline(shape.data(), int(shape.size()), rgba(0.55f, 0.72f, 1, 0.35f), ImDrawFlags_Closed, 1.3f * scale);
            focus_ring(h, shape);
            // the icon's badge
            const ImVec2 badge = {a.x + k + 30 * scale, a.y + 36 * scale};
            draw->AddCircleFilled(badge, 24 * scale, rgba(0.02f, 0.06f, 0.15f, 0.92f), 32);
            draw->AddCircle(badge, 24 * scale, on ? cyan : rgba(0.45f, 0.70f, 1, 0.6f), 32, 2 * scale);
            // title and its line
            const float tx = badge.x + 34 * scale;
            italic(draw, display_, fit(display_, 34 * scale, title, b.x - tx - 12 * scale), {tx, a.y + 18 * scale}, white, title, 0.18f);
            draw->AddText(body_, 17 * scale, {a.x + k + 8 * scale, a.y + 68 * scale}, rgba(0.88f, 0.93f, 1), line);
            // the chip at its foot
            const float cs = 14.5f * scale, cx = a.x + 14 * scale, cy0 = b.y - 37 * scale;
            const ImVec2 cw = measure(body_, cs, chip);
            const float dot = chip_dot ? 16 * scale : 0;
            const auto pill = rounded({cx, cy0}, {cx + cw.x + dot + 22 * scale, cy0 + 24 * scale}, 0, 12 * scale);
            fill(draw, pill, rgba(0.01f, 0.04f, 0.10f, 0.8f), rgba(0.01f, 0.04f, 0.10f, 0.8f), cy0, cy0 + 24 * scale);
            if (chip_dot) draw->AddCircleFilled({cx + 15 * scale, cy0 + 12 * scale}, 4.5f * scale, chip_col);
            draw->AddText(body_, cs, {cx + 11 * scale + dot, cy0 + 12 * scale - cs * 0.55f}, chip_col, chip);
            return std::pair<Hit, ImVec4>(h, ImVec4(a.x, a.y, b.x, b.y));
        };
        // Solo
        {
            const auto [h, box] = card("##solo_card", left, tab_ == Tab::Solo, false, TX("Solo"),
                                       TX("Race the clock\nand the field"), TX("3 courses · Time Attack"), muted, false);
            if (h.clicked || h.focused) tab_ = Tab::Solo;
            person(draw, {box.x + k + 30 * scale, box.y + 38 * scale}, 30 * scale, cyan);
            speedometer(draw, {box.z - k - 50 * scale, box.w - 50 * scale}, 42 * scale, scale, time);
        }
        // Online
        {
            const float x = left + card_w + gap;
            const auto [h, box] = card("##online_card", x, tab_ == Tab::Online, true, TX("Online"),
                                       TX("Race up to 8 players\nin real time"), TX("8 players · invite code"), muted, false);
            if (h.focused) tab_ = Tab::Online;
            if (h.clicked) {
                tab_ = Tab::Online;
                if (online.name.empty()) ask_name_ = true;
            }
            const ImVec2 badge = {box.x + k + 30 * scale, box.y + 36 * scale};
            person(draw, {badge.x + 7 * scale, badge.y - 1 * scale}, 22 * scale, rgba(0.30f, 0.70f, 0.95f));
            person(draw, {badge.x - 5 * scale, badge.y + 4 * scale}, 25 * scale, cyan);
            wifi(draw, {badge.x + 15 * scale, badge.y - 15 * scale}, 14 * scale, cyan);
            globe(draw, {box.z - k - 50 * scale, box.w - 50 * scale}, 42 * scale, scale, time);
        }
    }

    // Rows: a label at the left, the choices or the field at the right.
    const float yc = top + 336 * scale;
    const float label_w = 150 * scale, fx = left + label_w, fw = panel - label_w;
    auto row_label = [&](const char *label, float y, float height, int icon = -1) {
        const float cy = y + height * 0.5f;
        float x = left;
        if (icon >= 0) {
            const ImVec2 c = px(left + 13 * scale, cy);
            if (icon == 0) gamepad(draw, c, 26 * scale, rgba(0.88f, 0.92f, 1), navy);
            else if (icon == 1) shifter(draw, c, 22 * scale, rgba(0.88f, 0.92f, 1));
            else bars(draw, c, 22 * scale, rgba(0.88f, 0.92f, 1));
            x += 36 * scale;
        }
        const float size = fit(body_, 21 * scale, label, label_w - (x - left) - 8 * scale);
        italic(draw, body_, size, px(x, cy - size * 0.55f), white, label, 0.14f);
    };
    // A text field in a dark, lit frame; a hint at its right ("Pasted").
    auto field = [&](const char *id, char *buf, size_t size, float y, float height, bool user_icon, const char *hint,
                     ImU32 hint_col, bool shorten_text, bool &active) {
        const ImVec2 a = px(fx, y), b = px(fx + fw, y + height);
        const auto shape = rounded(a, b, 0, 6 * scale);
        fill(draw, shape, rgba(0.01f, 0.03f, 0.08f, 0.94f), rgba(0.02f, 0.05f, 0.12f, 0.94f), a.y, b.y);
        float tx = a.x + 14 * scale;
        if (user_icon) {
            person(draw, {tx + 11 * scale, (a.y + b.y) * 0.5f}, 24 * scale, white);
            tx += 34 * scale;
        }
        const bool tick = hint && hint_col == rgba(0.25f, 0.88f, 0.48f);
        const float hint_w = hint ? measure(body_, 14.5f * scale, hint).x + (tick ? 40 : 22) * scale : 12 * scale;
        const float font = 20 * scale;
        ImGui::PushFont(body_, font);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, 0u);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0u);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0u);
        const bool hide = shorten_text && !active;
        ImGui::PushStyleColor(ImGuiCol_Text, hide ? 0u : white);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, (height - font) * 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::SetCursorScreenPos({tx, a.y});
        ImGui::SetNextItemWidth(b.x - tx - hint_w);
        const bool changed = ImGui::InputText(id, buf, size);
        active = ImGui::IsItemActive();
        const bool focused = nav_focus() || active;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
        if (hide) {
            const std::string shown = shorten(body_, font, buf, b.x - tx - hint_w);
            draw->AddText(body_, font, {tx, (a.y + b.y - font) * 0.5f}, white, shown.c_str());
        }
        glow(draw, shape, neon_blue, focused ? 1.f : 0.5f, scale);
        if (nav_focus()) draw->AddPolyline(shape.data(), int(shape.size()), focus_col, ImDrawFlags_Closed, 2.f * scale);
        if (hint) {
            const float hx = b.x - hint_w + 8 * scale, cy = (a.y + b.y) * 0.5f;
            draw->AddText(body_, 14.5f * scale, {hx, cy - 8 * scale}, hint_col, hint);
            if (tick) {
                const float tx0 = hx + measure(body_, 14.5f * scale, hint).x + 7 * scale;
                const ImVec2 v[] = {{tx0, cy}, {tx0 + 4 * scale, cy + 4 * scale}, {tx0 + 11 * scale, cy - 5 * scale}};
                draw->AddPolyline(v, 3, hint_col, ImDrawFlags_None, 2 * scale);
            }
        }
        return changed;
    };
    // A text link (underlined when hovered or chosen).
    auto link = [&](const char *label, float x, float y, ImU32 col, float size = 15.5f) {
        const ImVec2 wide = measure(body_, size * scale, label);
        const ImVec2 a = px(x, y), b = {a.x + wide.x, a.y + wide.y};
        const Hit h = hit(label, {a.x - 2 * scale, a.y - 2 * scale}, {b.x + 2 * scale, b.y + 2 * scale});
        italic(draw, body_, size * scale, a, col, label, 0.12f);
        if (h.focused || h.hovered) draw->AddLine({a.x, b.y + 1 * scale}, {b.x, b.y + 1 * scale}, h.focused ? focus_col : col, 1.5f * scale);
        return std::pair<bool, float>(h.clicked, wide.x);
    };

    const Screen screen = loading ? Screen::Loading : has_race ? Screen::Paused : Screen::Ready;
    const bool focus_primary = screen != screen_;
    screen_ = screen;
    const float main_y = yc + 206 * scale, main_h = 76 * scale, quit_y = yc + 294 * scale, quit_h = 38 * scale;
    // The call to action: yellow, a play sign, a checkered end; dark for one
    // that only goes back.
    auto main_button = [&](const char *label, bool go) {
        if (focus_primary) ImGui::SetKeyboardFocusHere();
        const ImVec2 a = px(left, main_y), b = px(left + panel, main_y + main_h);
        const Hit h = hit(label, a, b);
        ImGui::SetItemDefaultFocus();
        const float k = main_h * 0.2f;
        const auto shape = rounded(a, b, k, 10 * scale);
        const float lift = h.held ? 0.f : (h.focused || h.hovered) ? 1.f : 0.f;
        if (go) {
            const float pulse = 0.5f + 0.5f * std::sin(time * 3.2f);
            for (int i = 4; i >= 1; --i) // a warm glow
                draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.70f, 0.15f, (0.10f + 0.06f * pulse) * (1 - (i - 1) / 4.f)),
                                  ImDrawFlags_Closed, (2 + i * 4.f) * scale);
            fill(draw, shape, rgba(1, 0.90f + 0.05f * lift, 0.36f + 0.1f * lift), rgba(1, 0.58f, 0.06f), a.y, b.y);
            // the checkered end, fading in toward the right
            ImGui::PushClipRect(a, b, true);
            const float cell = main_h / 4;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 4; ++j) {
                    if ((i + j) % 2) continue;
                    const float x = b.x - k - (8 - i) * cell, y = a.y + j * cell;
                    const float shift = (main_h - (y - a.y)) * 0.2f;
                    const float alpha = 0.55f * i / 7.f;
                    draw->AddQuadFilled({x + shift, y}, {x + cell + shift, y}, {x + cell + shift - cell * 0.2f, y + cell},
                                        {x + shift - cell * 0.2f, y + cell}, rgba(0.10f, 0.07f, 0.02f, alpha));
                }
            ImGui::PopClipRect();
            draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.95f, 0.65f, 0.9f), ImDrawFlags_Closed, 2 * scale);
        } else {
            fill(draw, shape, rgba(0.08f + 0.05f * lift, 0.14f + 0.06f * lift, 0.28f + 0.08f * lift, 0.95f),
                 rgba(0.04f, 0.07f, 0.16f, 0.95f), a.y, b.y);
            glow(draw, shape, neon_blue, 0.5f, scale);
        }
        focus_ring(h, shape);
        const float size = fit(body_, 44 * scale, label, panel - 200 * scale);
        const ImVec2 wide = measure(body_, size, label);
        const float total = wide.x + 46 * scale;
        const float x0 = a.x + (panel - total) * 0.5f, cy = (a.y + b.y) * 0.5f;
        const ImU32 col = go ? ink : white;
        if (go) draw->AddTriangleFilled({x0, cy - 15 * scale}, {x0 + 24 * scale, cy}, {x0, cy + 15 * scale}, col);
        italic(draw, body_, size, {x0 + (go ? 46 * scale : 23 * scale), cy - size * 0.55f}, col, label, 0.16f);
        return h.clicked;
    };
    // Quit, small, under it.
    auto small_button = [&](const char *label, float x, float width, bool exit) {
        const ImVec2 a = px(x, quit_y), b = px(x + width, quit_y + quit_h);
        const Hit h = hit(label, a, b);
        const auto shape = rounded(a, b, quit_h * 0.25f, 6 * scale);
        const float lift = h.focused || h.hovered ? 1.f : 0.f;
        fill(draw, shape, rgba(0.07f + 0.05f * lift, 0.13f + 0.06f * lift, 0.26f + 0.08f * lift, 0.92f), rgba(0.03f, 0.06f, 0.14f, 0.92f),
             a.y, b.y);
        draw->AddPolyline(shape.data(), int(shape.size()), rgba(0.45f, 0.75f, 1, 0.55f), ImDrawFlags_Closed, 1.4f * scale);
        focus_ring(h, shape);
        const float size = fit(body_, 20 * scale, label, width - 50 * scale);
        const ImVec2 wide = measure(body_, size, label);
        const float total = wide.x + (exit ? 34 * scale : 0);
        const float x0 = a.x + (width - total) * 0.5f, cy = (a.y + b.y) * 0.5f;
        if (exit) exit_icon(draw, {x0 + 10 * scale, cy}, 20 * scale, white, 2 * scale);
        italic(draw, body_, size, {x0 + (exit ? 34 * scale : 0), cy - size * 0.55f}, white, label, 0.14f);
        return h.clicked;
    };

    bool room_shown = false;

    if (loading) {
        // Start lights fill one by one while the race is prepared.
        const ImVec2 a = px(left, yc), b = px(left + panel, yc + 150 * scale);
        draw->AddRectFilled(a, b, rgba(0.02f, 0.04f, 0.09f, 0.85f), 10 * scale);
        draw->AddText(body_, 14 * scale, {a.x + 18 * scale, a.y + 14 * scale}, muted, TX("Preparing the race..."));
        const float cycle = std::fmod(time * 1.25f, 7.f);
        for (int i = 0; i < 5; ++i) {
            const float cx = a.x + panel * (i + 0.5f) / 5, cy = a.y + 90 * scale;
            const bool lit = cycle >= i + 1 && cycle < 6;
            const bool go = cycle >= 6;
            const ImU32 lamp = go ? rgba(0.25f, 1, 0.45f) : lit ? rgba(1, 0.18f, 0.14f) : rgba(0.20f, 0.07f, 0.08f);
            if (lit || go) draw->AddCircleFilled({cx, cy}, 26 * scale, go ? rgba(0.25f, 1, 0.45f, 0.18f) : rgba(1, 0.2f, 0.1f, 0.2f));
            draw->AddCircleFilled({cx, cy}, 19 * scale, lamp);
            draw->AddCircleFilled({cx - 6 * scale, cy - 6 * scale}, 5 * scale, rgba(1, 1, 1, lit || go ? 0.45f : 0.08f));
        }
        if (main_button(TX("Back"), false)) action = Cancel;
    } else if (solo_tab) {
        // The cabinet's language and difficulty, set here; then, as at the arcade, the player
        // chooses the course, the gearbox and Time Attack (Start held) on the game's own screens.
        {
            // the same rows as the Online card's: its invite code, its "or" line, its new room
            const float cy0 = yc + 20 * scale, cy1 = yc + 106 * scale, line = yc + 88 * scale;
            row_label(TX("Country"), cy0, 44 * scale);
            const char *countries[] = {TX("USA (English)"), TX("Japan (Japanese)"), TX("Export")};
            const float third = (fw - 16 * scale) / 3;
            for (int i = 0; i < 3; ++i) {
                const ImVec2 a = px(fx + i * (third + 8 * scale), cy0), b = {a.x + third, a.y + 44 * scale};
                char id[24];
                std::snprintf(id, sizeof id, "##country%d", i);
                if (neon::choice(draw, body_, scale, id, countries[i], a, b, solo_country_ == i, 17)) solo_country_ = i;
            }
            draw->AddLine(px(left, line), px(left + panel, line), rgba(0.6f, 0.75f, 1, 0.25f), 1.f * scale);
            row_label(TX("Difficulty"), cy1, 46 * scale);
            const char *levels[] = {TX("Easy"), TX("Normal"), TX("Hard"), TX("Hardest")};
            const float quarter = (fw - 24 * scale) / 4;
            for (int i = 0; i < 4; ++i) {
                const ImVec2 a = px(fx + i * (quarter + 8 * scale), cy1), b = {a.x + quarter, a.y + 46 * scale};
                char id[24];
                std::snprintf(id, sizeof id, "##difficulty%d", i);
                if (neon::choice(draw, body_, scale, id, levels[i], a, b, solo_difficulty_ == i, 17)) solo_difficulty_ = i;
            }
        }
        if (main_button(has_race ? TX("Resume") : TX("Play"), true)) {
            if (has_race) action = Resume;
            else if (in_room || connecting) ask_leave_ = true; // a solo race takes the player out of the room
            else action = Play;
        }
        if (has_race && small_button(TX("New race"), left, 230 * scale, false)) action = Play;
    } else if (in_room || connecting) {
        room_shown = true;
        const ImVec2 a = px(left, yc), b = px(left + panel, yc + 150 * scale);
        const auto shape = rounded(a, b, 0, 10 * scale);
        fill(draw, shape, rgba(0.05f, 0.10f, 0.22f, 0.92f), rgba(0.03f, 0.06f, 0.14f, 0.92f), a.y, b.y);
        glow(draw, shape, neon_blue, 0.45f, scale);
        if (connecting) {
            draw->AddText(body_, 17 * scale, {a.x + 18 * scale, a.y + 18 * scale}, white, TX("Connecting to the room..."));
            const int dots = int(time * 3) % 4;
            for (int i = 0; i < dots; ++i) draw->AddCircleFilled({a.x + (24 + i * 14) * scale, a.y + 62 * scale}, 4 * scale, cyan);
            if (!online.error.empty())
                draw->AddText(body_, 14 * scale, {a.x + 18 * scale, a.y + 84 * scale}, alarm, online.error.c_str());
            if (main_button(TX("Cancel"), false)) action = LeaveRoom;
        } else {
            char line[96];
            std::snprintf(line, sizeof line, TX("Room  %d / %d"), online.joined, online::kMaxPlayers);
            italic(draw, body_, 19 * scale, {a.x + 18 * scale, a.y + 12 * scale}, white, line, 0.12f);
            const std::string code = shorten(body_, 15.5f * scale, online.code, panel * 0.6f);
            const ImVec2 cw = measure(body_, 15.5f * scale, code.c_str());
            draw->AddText(body_, 15.5f * scale, {b.x - cw.x - 18 * scale, a.y + 15 * scale}, cyan, code.c_str());
            // eight places in the cars' colours: the owner's crowned, ours ringed
            const float slot_gap = 6 * scale, slot_w = (panel - 36 * scale - 7 * slot_gap) / 8, sy = a.y + 44 * scale;
            for (int i = 0; i < 8; ++i) {
                const ImVec2 sa = {a.x + 18 * scale + i * (slot_w + slot_gap), sy}, sb = {sa.x + slot_w, sy + 32 * scale};
                const auto s = rounded(sa, sb, 5 * scale, 4 * scale);
                const bool taken = i < int(online.names.size());
                const uint32_t c = online_car_colour(i + 1);
                if (taken) fill(draw, s, rgb24(c), rgb24(c, 1), sa.y, sb.y);
                else fill(draw, s, rgba(1, 1, 1, 0.06f), rgba(1, 1, 1, 0.06f), sa.y, sb.y);
                if (taken && i + 1 == online.me) draw->AddPolyline(s.data(), int(s.size()), white, ImDrawFlags_Closed, 2.2f * scale);
                const float luma = ((c >> 16 & 255) * 0.3f + (c >> 8 & 255) * 0.59f + (c & 255) * 0.11f) / 255.f;
                const ImU32 tc = !taken ? rgba(1, 1, 1, 0.25f) : luma > 0.6f ? ink : white;
                char n[4];
                std::snprintf(n, sizeof n, "%d", i + 1);
                const ImVec2 nw = measure(body_, 14 * scale, n);
                const float cx = (sa.x + sb.x) * 0.5f;
                if (taken && i == 0) {
                    crown(draw, {cx - nw.x * 0.5f - 7 * scale, (sa.y + sb.y) * 0.5f}, 11 * scale, tc);
                    draw->AddText(body_, 14 * scale, {cx - nw.x * 0.5f + 2 * scale, sa.y + 8 * scale}, tc, n);
                } else {
                    draw->AddText(body_, 14 * scale, {cx - nw.x * 0.5f, sa.y + 8 * scale}, tc, n);
                }
            }
            // who, and what the room waits for
            std::string who;
            for (size_t i = 0; i < online.names.size(); ++i) {
                if (!who.empty()) who += ", ";
                char unnamed[32];
                std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), int(i) + 1);
                who += online.names[i].empty() ? unnamed : online.names[i];
            }
            char state[160];
            ImU32 state_col = amber;
            const std::string owner_name = online.names.empty() || online.names[0].empty() ? TX("the room's owner") : online.names[0];
            if (!online.racing) {
                if (online.owner)
                    std::snprintf(state, sizeof state, "%s", online.joined < online::kMinPlayers ? TX("Waiting for at least one more player.")
                                                                                              : TX("Start the race when everyone is in."));
                else std::snprintf(state, sizeof state, TX("Waiting for %s to start the race."), owner_name.c_str());
            } else if (!online.me_in_race) {
                std::snprintf(state, sizeof state, "%s", TX("A race is on: you will join the next one."));
            } else {
                std::snprintf(state, sizeof state, "%s", online.game_running ? TX("Online race in progress.") : TX("Starting the race..."));
                state_col = green;
            }
            draw->AddText(body_, 15.5f * scale, {a.x + 18 * scale, a.y + 86 * scale}, muted, who.c_str(), nullptr, panel - 36 * scale);
            draw->AddText(body_, 17 * scale, {a.x + 18 * scale, a.y + 112 * scale}, state_col, state, nullptr, panel - 36 * scale);

            float lx = left;
            const auto copy = link(copied_ ? TX("Copied!") : TX("Copy the invite code"), lx, yc + 162 * scale, cyan);
            if (copy.first) {
                ImGui::SetClipboardText(online.code.c_str());
                seen_clip_ = online.code; // not pasted back into the Join field
                copied_ = true;
            }
            lx += copy.second + 26 * scale;
            const auto details = link(TX("Room details..."), lx, yc + 162 * scale, cyan);
            if (details.first) action = Online;
            lx += details.second + 26 * scale;
            if (link(TX("Leave the room"), lx, yc + 162 * scale, alarm).first) action = LeaveRoom;

            if (online.owner && !online.racing) {
                if (online.joined >= online::kMinPlayers) {
                    if (main_button(TX("Start the race"), true)) action = StartRace;
                } else {
                    main_button(TX("Waiting for players"), false);
                }
            } else if (online.racing && online.me_in_race && online.game_running) {
                if (main_button(TX("Resume the race"), true)) action = Resume;
            } else if (main_button(TX("Open the room"), false)) {
                action = Online;
            }
        }
    } else {
        // Join with a code (pasted on its own when the clipboard holds one), or create a room
        if (!invite_active_ && time - clipboard_at_ > 1.0) {
            clipboard_at_ = time;
            if (const char *clip = ImGui::GetClipboardText()) {
                const std::string c = trim(clip);
                if (c != seen_clip_) {
                    seen_clip_ = c;
                    if (c.size() < sizeof invite_ && online::decode_invite(c)) paste_invite(c);
                }
            }
        }
        // Join with a code, or create a room: its settings (name, server,
        // race) are the next screen's (app/online_menu.cpp)
        const bool valid = online::decode_invite(invite_).has_value();
        const float iy = yc + 20 * scale;
        row_label(TX("Invite code"), iy, 44 * scale);
        if (field("##invite", invite_, sizeof invite_, iy, 44 * scale, false,
                  pasted_ && valid ? TX("Pasted") : !*invite_ ? nullptr : valid ? TX("Valid") : TX("Incomplete"),
                  valid ? green : amber, true, invite_active_)) {
            pasted_ = false;
            invite_text_ = invite_;
        }
        {
            const float y = yc + 88 * scale;
            const ImVec2 ow = measure(body_, 14 * scale, TX("or"));
            const float mid = left + panel * 0.5f;
            draw->AddLine(px(left, y), px(mid - ow.x * 0.5f - 12 * scale, y), rgba(0.6f, 0.75f, 1, 0.25f), 1.f * scale);
            draw->AddLine(px(mid + ow.x * 0.5f + 12 * scale, y), px(left + panel, y), rgba(0.6f, 0.75f, 1, 0.25f), 1.f * scale);
            draw->AddText(body_, 14 * scale, px(mid - ow.x * 0.5f, y - 9 * scale), muted, TX("or"));
        }
        const float cy = yc + 106 * scale;
        row_label(TX("New room"), cy, 46 * scale);
        if (neon::blue_button(draw, body_, scale, TX("Create a new room"), TX("Create a new room"), px(fx, cy), px(fx + fw, cy + 46 * scale), 1))
            action = Online;
        draw->AddText(body_, 15.5f * scale, px(fx + 4 * scale, cy + 54 * scale), muted, TX("Then your name, the server and the race settings."),
                      nullptr, fw);
        if (!online.error.empty())
            draw->AddText(body_, 14 * scale, px(left, yc + 186 * scale), alarm, online.error.c_str(), nullptr, panel);
        ImGui::BeginDisabled(!valid); // lit once a code is there
        if (main_button(TX("Join the room"), true) && valid) {
            invite_text_ = invite_;
            if (online.name.empty()) ask_name_ = true, pending_ = JoinRoom;
            else action = JoinRoom;
        }
        ImGui::EndDisabled();
    }
    if (!room_shown) copied_ = false;

    if (message && *message) {
        const float y = has_race && solo_tab ? quit_y + quit_h + 12 * scale : quit_y; // under New race, else under the main button
        ImGui::SetCursorScreenPos(px(left + 12 * scale, y));
        ImGui::PushFont(body_, 15 * scale);
        ImGui::PushTextWrapPos(px(left + panel, 0).x);
        ImGui::TextColored({1, 0.74f, 0.42f, 1}, "%s", message);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        draw->AddRectFilled(px(left, y + 2 * scale), {px(left + 4 * scale, 0).x, ImGui::GetItemRectMax().y - 2 * scale}, orange);
    }

    // The status pill and the settings, top right.
    {
        const float ph = 42 * scale, py = 24 * scale, margin = 28 * scale, gear_w = 58 * scale;
        // Quit at the corner, Settings beside it: icons in the same boxes
        auto corner = [&](const char *id, float right, bool quit) {
            const ImVec2 a = px(w - right - gear_w, py), b = px(w - right, py + ph);
            const Hit h = hit(id, a, b);
            const auto shape = rounded(a, b, ph * 0.22f, 8 * scale);
            fill(draw, shape, rgba(0.05f, 0.10f, 0.22f, 0.92f), rgba(0.03f, 0.06f, 0.14f, 0.92f), a.y, b.y);
            glow(draw, shape, quit ? ImVec4(1, 0.36f, 0.36f, 1) : neon_blue, h.focused || h.hovered ? 1.f : 0.6f, scale);
            focus_ring(h, shape);
            const ImVec2 c = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            if (quit) exit_icon(draw, {c.x - 1 * scale, c.y}, 22 * scale, white, 2.2f * scale);
            else gear(draw, c, 26 * scale, white, rgba(0.05f, 0.10f, 0.22f));
            if (h.hovered) ImGui::SetTooltip("%s", id);
            return std::pair<bool, ImVec2>(h.clicked, a);
        };
        if (corner(TX("Quit"), margin, true).first) action = Quit;
        const auto settings_box = corner(TX("Settings"), margin + gear_w + 12 * scale, false);
        if (settings_box.first) action = Settings;
        const ImVec2 ga = settings_box.second, gb = {ga.x + gear_w, ga.y + ph};

        const float fs = 18.5f * scale;
        const char *who = online.name.empty() ? TX("Set your name") : online.name.c_str();
        char ms[24];
        if (online.latency_ms >= 0) std::snprintf(ms, sizeof ms, TX("%d ms"), online.latency_ms);
        else std::snprintf(ms, sizeof ms, "--");
        const float w1 = 26 * scale + measure(body_, fs, status_word).x, w2 = 30 * scale + measure(body_, fs, who).x + 24 * scale,
                    w3 = measure(body_, fs, ms).x;
        const float pad = 20 * scale, sep = 26 * scale;
        const float pill_w = pad * 2 + w1 + w2 + w3 + sep * 2;
        const ImVec2 a = {ga.x - 14 * scale - pill_w, ga.y}, b = {ga.x - 14 * scale, gb.y};
        const Hit h = hit("##pill", a, b);
        const auto shape = rounded(a, b, ph * 0.22f, ph * 0.45f);
        fill(draw, shape, rgba(0.04f, 0.08f, 0.18f, 0.88f), rgba(0.02f, 0.05f, 0.12f, 0.88f), a.y, b.y);
        glow(draw, shape, neon_blue, h.focused || h.hovered ? 1.f : 0.6f, scale);
        focus_ring(h, shape);
        float x = a.x + pad;
        const float cy = (a.y + b.y) * 0.5f;
        const float beat = online.probe == HomeOnline::Probe::Testing ? 0.5f + 0.5f * std::sin(time * 6) : 1.f;
        draw->AddCircleFilled({x + 6 * scale, cy}, 6.5f * scale, (status_col & 0x00ffffffu) | (ImU32(255 * (0.5f + 0.5f * beat)) << 24));
        italic(draw, body_, fs, {x + 20 * scale, cy - fs * 0.55f}, status_col, status_word, 0.12f);
        x += w1 + sep * 0.5f;
        draw->AddLine({x, a.y + 10 * scale}, {x, b.y - 10 * scale}, rgba(1, 1, 1, 0.2f), 1 * scale);
        x += sep * 0.5f;
        draw->AddCircle({x + 11 * scale, cy}, 11 * scale, white, 20, 1.6f * scale);
        person(draw, {x + 11 * scale, cy + 1 * scale}, 15 * scale, white);
        italic(draw, body_, fs, {x + 30 * scale, cy - fs * 0.55f}, white, who, 0.12f);
        chevron_down(draw, {x + w2 - 8 * scale, cy}, 16 * scale, white, 2 * scale);
        x += w2 + sep * 0.5f;
        draw->AddLine({x, a.y + 10 * scale}, {x, b.y - 10 * scale}, rgba(1, 1, 1, 0.2f), 1 * scale);
        x += sep * 0.5f;
        italic(draw, body_, fs, {x, cy - fs * 0.55f}, online.latency_ms >= 0 ? green : muted, ms, 0.12f);
        if (h.clicked) ask_name_ = true; // the player's name (no accounts: nothing else to choose)
    }

    // Bottom bar: a checkered run, the version and the Online state, the display.
    const float bottom = h - 46 * scale;
    {
        draw->AddRectFilledMultiColor(px(0, bottom - 34 * scale), px(w, h), rgba(0.02f, 0.04f, 0.09f, 0),
                                      rgba(0.02f, 0.04f, 0.09f, 0), rgba(0.02f, 0.04f, 0.09f, 0.7f), rgba(0.02f, 0.04f, 0.09f, 0.7f));
        draw->AddRectFilledMultiColor(px(left, bottom + 2 * scale), px(w - left, bottom + 3 * scale), rgba(0.4f, 0.75f, 1, 0.55f),
                                      rgba(0.4f, 0.75f, 1, 0.1f), rgba(0.4f, 0.75f, 1, 0.1f), rgba(0.4f, 0.75f, 1, 0.55f));
        const float cell = 6 * scale, base = bottom + 14 * scale;
        for (int i = 0; i < 12; ++i)
            for (int j = 0; j < 2; ++j)
                if ((i + j) % 2 == 0) {
                    const float x = left + i * cell + (1 - j) * cell * 0.5f, y = base + j * cell;
                    draw->AddQuadFilled(px(x + cell * 0.5f, y), px(x + cell * 1.5f, y), px(x + cell, y + cell), px(x, y + cell),
                                        rgba(1, 1, 1, 0.75f * (1 - i / 14.f)));
                }
        char version[96];
        std::snprintf(version, sizeof version, TX("v%s (%s)"), DAYTONA_APP_VERSION, DAYTONA_APP_COMMIT);
        float x = left + 12 * cell + 22 * scale;
        italic(draw, body_, 14 * scale, px(x, base - 1 * scale), white, version, 0.12f);
        x += measure(body_, 14 * scale, version).x + 18 * scale;
        draw->AddLine(px(x, base - 2 * scale), px(x, base + 14 * scale), rgba(1, 1, 1, 0.3f), 1 * scale);
        x += 18 * scale;
        // ready: a name, a server, and it answered (or a room)
        const char *ready;
        ImU32 ready_col;
        if (online.name.empty()) ready = TX("SET A NAME"), ready_col = amber;
        else if (in_room || connecting) ready = TX("ONLINE READY"), ready_col = green;
        else if (online.server.empty()) ready = TX("NO SERVER"), ready_col = amber;
        else if (online.probe == HomeOnline::Probe::Ok) ready = TX("ONLINE READY"), ready_col = green;
        else if (online.probe == HomeOnline::Probe::Offline) ready = TX("SERVER OFFLINE"), ready_col = alarm;
        else if (online.probe == HomeOnline::Probe::Version) ready = TX("VERSION MISMATCH"), ready_col = alarm;
        else ready = TX("TESTING THE SERVER"), ready_col = amber;
        wifi(draw, px(x + 10 * scale, base + 4 * scale), 20 * scale, ready_col);
        italic(draw, body_, 14 * scale, px(x + 28 * scale, base - 1 * scale), ready_col, ready, 0.12f);

        char display[80];
        const auto ratio = ImGui::GetIO().DisplayFramebufferScale;
        std::snprintf(display, sizeof display, TX("SDR   /   %d × %d"), int(w * ratio.x), int(h * ratio.y));
        const ImVec2 wide = measure(body_, 14 * scale, display);
        italic(draw, body_, 14 * scale, px(w - left - wide.x, base - 1 * scale), white, display, 0.12f);
        monitor(draw, px(w - left - wide.x - 22 * scale, base + 6 * scale), 22 * scale, white, 1.8f * scale);
    }

    // A new version is out (app/update_check.h): a card above the bottom
    // bar, at the right, over the scene; what to do, and its release page.
    // Players build their own game (rules.md, 22): source, never a download.
    if (!update_.empty() && !loading) {
        const float pad = 18 * scale, title_s = 24 * scale, body_s = 16.5f * scale, button_h = 38 * scale;
        const float card_w = std::min(440 * scale, std::max(300 * scale, w - left - panel - 60 * scale - left));
        const float wrap = card_w - 2 * pad;
        char title[96];
        std::snprintf(title, sizeof title, TX("NEW VERSION %s"), update_.c_str());
        const char *line = TX("Update your copy of the source (git pull, or download it again) and build the game again.");
        const float line_h = body_->CalcTextSizeA(body_s, FLT_MAX, wrap, line).y;
        const float card_h = pad + title_s + 10 * scale + line_h + 16 * scale + button_h + pad;
        const ImVec2 b = px(w - left, bottom - 40 * scale), a = {b.x - card_w, b.y - card_h};
        const auto shape = rounded(a, b, 10 * scale, 12 * scale);
        fill(draw, shape, rgba(0.07f, 0.13f, 0.27f, 0.95f), rgba(0.02f, 0.05f, 0.13f, 0.95f), a.y, b.y);
        glow(draw, shape, ImVec4(1, 0.80f, 0.28f, 1), 0.45f + 0.15f * std::sin(time * 2.f), scale);
        italic(draw, display_, fit(display_, title_s, title, wrap), {a.x + pad, a.y + pad}, amber, title, 0.18f);
        draw->AddText(body_, body_s, {a.x + pad, a.y + pad + title_s + 10 * scale}, rgba(0.88f, 0.93f, 1), line, nullptr, wrap);
        const float by = b.y - pad - button_h, bw = (wrap - 12 * scale) * 0.5f;
        if (neon::blue_button(draw, body_, scale, "##release_page", TX("Release page"), {a.x + pad, by},
                              {a.x + pad + bw, by + button_h}, 0, 17.f))
            action = OpenRelease;
        if (neon::outline_button(draw, body_, scale, TX("Dismiss"), {b.x - pad - bw, by}, {b.x - pad, by + button_h},
                                 ImVec4(0.55f, 0.72f, 1, 1), 17.f))
            action = DismissUpdate;
    }

    // The player's name: asked once, or from the pill.
    const ImVec2 middle = {view->Pos.x + w * 0.5f, view->Pos.y + h * 0.5f};
    if (ask_name_) {
        ask_name_ = false;
        std::string start = online.name;
        if (start.empty()) // a suggestion: the computer's user name
            for (const char *var : {"USER", "USERNAME"})
                if (const char *v = std::getenv(var); v && *v) {
                    start = online::clean_name(v);
                    break;
                }
        std::snprintf(ask_, sizeof ask_, "%s", start.c_str());
        ImGui::OpenPopup("##ask_name");
    }
    if (ask_leave_) {
        ask_leave_ = false;
        ImGui::OpenPopup("##ask_leave");
    }
    ImGui::PushFont(body_, 18 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {26 * scale, 22 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12 * scale, 10 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12 * scale, 12 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * scale);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.04f, 0.08f, 0.18f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.38f, 0.80f, 1, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0.02f, 0.06f, 0.6f));
    const ImGuiWindowFlags modal = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(middle, ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("##ask_name", nullptr, modal)) {
        ImGui::TextUnformatted(TX("Your name"));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.90f, 1));
        ImGui::TextUnformatted(TX("The other players see it above your car."));
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(380 * scale);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##asked", ask_, sizeof ask_, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string clean = online::clean_name(ask_);
        ImGui::BeginDisabled(clean.empty());
        const bool ok = ImGui::Button(TX("OK"), {184 * scale, 0}) || (enter && !clean.empty());
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = ImGui::Button(TX("Cancel"), {184 * scale, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ok) {
            name_text_ = clean;
            std::snprintf(name_, sizeof name_, "%s", clean.c_str());
            action = SetName;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            pending_ = Stay;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(middle, ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("##ask_leave", nullptr, modal)) {
        ImGui::TextUnformatted(TX("Leave the room?"));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.90f, 1));
        ImGui::TextUnformatted(TX("A solo race takes you out of the online room."));
        ImGui::PopStyleColor();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(TX("Stay in the room"), {220 * scale, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button(TX("Leave and race"), {220 * scale, 0})) {
            action = Play;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(5);
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar(2);
    return action;
}

} // namespace app
