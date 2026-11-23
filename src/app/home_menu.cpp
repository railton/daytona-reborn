#include "app/home_menu.h"
#include "app/i18n.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>

namespace app {

using Course = rt::RaceCourse;

namespace {

ImFont *load_font(ImFontAtlas *atlas, std::initializer_list<std::string> paths) {
    for (const std::string &path : paths)
        if (std::ifstream(path, std::ios::binary))
            if (auto *font = atlas->AddFontFromFileTTF(path.c_str())) return font;
    return nullptr;
}

// The home screen's palette: night-blue panels, white type, a yellow-orange
// call to action, sky blue for what is chosen, a red accent.
ImU32 rgba(float r, float g, float b, float a = 1) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }

// Buttons and tags lean forward like the type: a parallelogram whose top edge
// is pushed right by `lean` of its height.
constexpr float lean = 0.24f;

void slab(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
    const float k = (b.y - a.y) * lean;
    const ImVec2 p[] = {{a.x + k, a.y}, {b.x, a.y}, {b.x - k, b.y}, {a.x, b.y}};
    const int first = draw->VtxBuffer.Size;
    draw->AddConvexPolyFilled(p, 4, top);
    if (top != bottom)
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, a, {a.x, b.y}, top, bottom);
}

void slab_outline(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
    const float k = (b.y - a.y) * lean;
    const ImVec2 p[] = {{a.x + k, a.y}, {b.x, a.y}, {b.x - k, b.y}, {a.x, b.y}};
    draw->AddPolyline(p, 4, color, ImDrawFlags_Closed, thickness);
}

// Text sheared into italics, optionally shaded top to bottom.
void italic(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 top, const char *text,
            float shear = 0.2f, ImU32 bottom = 0) {
    const int first = draw->VtxBuffer.Size;
    draw->AddText(font, size, at, top, text);
    const int last = draw->VtxBuffer.Size;
    const float base = at.y + size * 0.8f;
    for (int i = first; i < last; ++i) draw->VtxBuffer[i].pos.x += (base - draw->VtxBuffer[i].pos.y) * shear;
    if (bottom)
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, last, at, {at.x, at.y + size}, top, bottom);
}

ImVec2 measure(ImFont *font, float size, const char *text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0, text);
}

// Shrinks a size so the text fits a width (long translations).
float fit(ImFont *font, float size, const char *text, float width) {
    const float wide = measure(font, size, text).x;
    return wide > width ? size * width / wide : size;
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

HomeMenu::Action HomeMenu::draw(bool has_race, bool loading, const char *message, float opacity) {
    const auto *view = ImGui::GetMainViewport();
    const float w = view->Size.x, h = view->Size.y;
    const float scale = std::max(0.13f, std::min(h / 900.f, w / 980.f));
    const float left = std::max(w * 0.052f, 31.f * scale);
    // Centred, but high enough that the tallest case (a race paused, with a
    // two-line message) clears the bottom bar, so the column never jumps.
    const float top = std::max(26.f * scale, std::min(h * 0.5f - 330.f * scale, h - 800.f * scale));
    const float panel_width = 372.f * scale;
    const float time = float(ImGui::GetTime());
    const auto px = [&](float x, float y) { return ImVec2(view->Pos.x + x, view->Pos.y + y); };
    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Daytona home", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    auto *draw = ImGui::GetWindowDrawList();
    const ImU32 white = rgba(0.97f, 0.98f, 1), muted = rgba(0.70f, 0.78f, 0.88f);
    const ImU32 ink = rgba(0.05f, 0.07f, 0.13f), red = rgba(0.93f, 0.22f, 0.20f);
    const ImU32 yellow = rgba(1, 0.83f, 0.20f), orange = rgba(1, 0.52f, 0.13f), sky = rgba(0.22f, 0.58f, 1);
    const ImVec4 course_tint[] = {{0.30f, 0.76f, 1, 1}, {1, 0.71f, 0.20f, 1}, {1, 0.33f, 0.30f, 1}};
    auto text = [&](ImFont *font, float size, float x, float y, ImU32 tint, const char *label) {
        draw->AddText(font, size * scale, px(x, y), tint, label);
    };

    // Whether a button shows as the keyboard/gamepad's current one.
    auto nav_focus = [] { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; };
    enum class Look { Plain, Chosen, Primary };
    // A leaning button; `label` is its id and its text. `under` is an optional
    // second line in small type.
    auto button = [&](const char *label, float x, float y, float width, float height, Look look,
                      float font_size = 16.f, const char *under = nullptr, ImVec4 accent = {0, 0, 0, 0}) {
        ImGui::SetCursorScreenPos(px(x, y));
        const bool clicked = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
        const bool focused = nav_focus(), hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const ImVec2 a = px(x, y), b = px(x + width, y + height);
        const float k = height * lean;
        const float lift = held ? 0.f : (focused || hovered) ? 1.f : 0.f;
        ImU32 fill_top, fill_bottom, label_color;
        if (look == Look::Primary) {
            const float pulse = 0.5f + 0.5f * std::sin(time * 3.2f);
            fill_top = rgba(1, 0.86f + 0.06f * lift, 0.24f + 0.12f * pulse * lift);
            fill_bottom = rgba(1, 0.50f + 0.08f * lift, 0.12f);
            label_color = ink;
        } else if (look == Look::Chosen) {
            const ImVec4 c = accent.w > 0 ? accent : ImVec4(0.22f, 0.58f, 1, 1);
            fill_top = rgba(std::min(1.f, c.x + 0.08f * lift), std::min(1.f, c.y + 0.08f * lift), std::min(1.f, c.z + 0.08f * lift));
            fill_bottom = rgba(c.x * 0.78f, c.y * 0.78f, c.z * 0.78f);
            label_color = accent.w > 0 ? ink : white;
        } else {
            fill_top = rgba(0.10f + 0.07f * lift, 0.15f + 0.08f * lift, 0.25f + 0.10f * lift, 0.88f);
            fill_bottom = rgba(0.05f + 0.05f * lift, 0.08f + 0.06f * lift, 0.15f + 0.08f * lift, 0.88f);
            label_color = rgba(0.88f, 0.92f, 1);
        }
        slab(draw, a, b, fill_top, fill_bottom);
        if (look == Look::Plain) slab_outline(draw, a, b, rgba(1, 1, 1, 0.10f + 0.25f * lift), 1.f);
        if (look == Look::Chosen && accent.w == 0) // a tick on the chosen one
            slab(draw, {a.x + 7 * scale, b.y - 5 * scale}, {a.x + 7 * scale + 22 * scale, b.y - 2.5f * scale}, yellow, yellow);
        if (focused || hovered) {
            slab_outline(draw, {a.x - 2, a.y - 2}, {b.x + 2, b.y + 2}, focused ? rgba(1, 0.9f, 0.35f) : rgba(1, 1, 1, 0.55f),
                         focused ? 2.5f * scale : 1.5f * scale);
            if (focused) { // an arcade cursor ahead of the button
                const float nudge = 3 * scale * (0.5f + 0.5f * std::sin(time * 6));
                const float cy = (a.y + b.y) * 0.5f, cx = a.x + k * 0.5f - 9 * scale - nudge;
                draw->AddTriangleFilled({cx - 8 * scale, cy - 7 * scale}, {cx, cy}, {cx - 8 * scale, cy + 7 * scale},
                                        rgba(1, 0.86f, 0.25f));
            }
        }
        const float room = width - k - 18 * scale;
        const float size = fit(body_, font_size * scale, label, room);
        const ImVec2 wide = measure(body_, size, label);
        const float lines = under ? size + 14 * scale : size;
        float tx = a.x + (width - wide.x) * 0.5f, ty = a.y + (height - lines) * 0.5f;
        if (look == Look::Primary) tx = a.x + k + 24 * scale;
        italic(draw, body_, size, {tx, ty}, label_color, label, 0.14f);
        if (under) {
            const float small = fit(body_, 11.5f * scale, under, room);
            const ImVec2 sub = measure(body_, small, under);
            draw->AddText(body_, small, {a.x + (width - sub.x) * 0.5f - 2 * scale, ty + size + 1 * scale},
                          look == Look::Chosen ? rgba(0.05f, 0.07f, 0.13f, 0.75f) : muted, under);
        }
        if (look == Look::Primary) { // chevrons running toward the edge
            const float cy = (a.y + b.y) * 0.5f, ch = height * 0.26f;
            for (int i = 0; i < 3; ++i) {
                const float phase = std::fmod(time * 1.6f - i * 0.22f + 3, 1.f);
                const float alpha = 0.25f + 0.75f * (phase < 0.5f ? 1 - phase * 2 : 0.f) * (0.5f + 0.5f * lift) + 0.2f;
                const float cx = b.x - k - 70 * scale + i * 17 * scale;
                const ImVec2 p[] = {{cx, cy - ch}, {cx + 9 * scale, cy - ch}, {cx + 9 * scale + ch * 0.75f, cy},
                                    {cx + 9 * scale, cy + ch}, {cx, cy + ch}, {cx + ch * 0.75f, cy}};
                draw->AddConcavePolyFilled(p, 6, rgba(0.05f, 0.07f, 0.13f, std::min(1.f, alpha)));
            }
        }
        return clicked;
    };

    // Tag line with a red-white-blue chevron run.
    {
        const ImU32 chevrons[] = {red, white, sky};
        for (int i = 0; i < 3; ++i) {
            const float x = left + i * 11 * scale;
            slab(draw, px(x, top - 1 * scale), px(x + 8 * scale, top + 12 * scale), chevrons[i], chevrons[i]);
        }
        text(body_, 12.5f, left + 42 * scale, top - 2 * scale, muted, TX("M O D E L   2    /    P C   E D I T I O N"));
    }
    // Title: italic, white into gold, over a hard shadow and speed streaks.
    {
        const float size = 92 * scale;
        const char *title = "DAYTONA";
        const ImVec2 at = px(left - 3 * scale, top + 16 * scale);
        const ImVec2 wide = measure(display_, size, title);
        for (int i = 0; i < 5; ++i) { // streaks off the trailing edge
            const float y = at.y + size * (0.26f + i * 0.13f);
            const float run = (60 + 34 * ((i * 7) % 5)) * scale;
            const float drift = std::fmod(time * 0.35f + i * 0.31f, 1.f);
            const float x0 = at.x + wide.x + 18 * scale + drift * 26 * scale;
            draw->AddRectFilledMultiColor({x0, y}, {x0 + run, y + 2.5f * scale}, rgba(1, 1, 1, 0.45f), rgba(1, 1, 1, 0),
                                          rgba(1, 1, 1, 0), rgba(1, 1, 1, 0.45f));
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
        slab(draw, {b.x + 4 * scale, a.y}, {b.x + 14 * scale, b.y}, yellow, orange);
        slab(draw, {b.x + 18 * scale, a.y}, {b.x + 24 * scale, b.y}, rgba(1, 0.83f, 0.20f, 0.5f), rgba(1, 0.52f, 0.13f, 0.5f));
        italic(draw, body_, size, {a.x + 22 * scale, a.y + (27 * scale - size) * 0.5f}, white, tag, 0.12f);
    }

    const unsigned course = std::min(unsigned(course_), 2u);
    const ImVec4 tint = course_tint[course];
    const char *tracks[] = {"Three Seven Speedway", "Dinosaur Canyon", "Seaside Street Galaxy"};
    const char *levels[] = {TX("Beginner"), TX("Advanced"), TX("Expert")};
    // The chosen course, like a timing-screen card.
    {
        const ImVec2 a = px(left, top + 166 * scale), b = px(left + panel_width, top + 240 * scale);
        draw->AddRectFilledMultiColor(a, b, rgba(0.04f, 0.07f, 0.14f, 0.78f), rgba(0.04f, 0.07f, 0.14f, 0.30f),
                                      rgba(0.04f, 0.07f, 0.14f, 0.30f), rgba(0.04f, 0.07f, 0.14f, 0.78f));
        draw->AddRectFilled(a, {a.x + 4 * scale, b.y}, ImGui::GetColorU32(tint));
        text(body_, 12.5f, left + 18 * scale, top + 176 * scale, muted, loading ? TX("Preparing the race...") : TX("Solo race"));
        const float size = fit(display_, 27 * scale, tracks[course], panel_width - 30 * scale);
        italic(draw, display_, size, px(left + 17 * scale, top + 198 * scale), white, tracks[course], 0.16f);
        // Difficulty: one to three chevrons.
        const char *level = levels[course];
        const ImVec2 wide = measure(body_, 12.5f * scale, level);
        const float right = left + panel_width - 12 * scale;
        for (int i = 0; i < 3; ++i) {
            const float x = right - (3 - i) * 12 * scale;
            const ImU32 c = i <= int(course) ? ImGui::GetColorU32(tint) : rgba(1, 1, 1, 0.16f);
            slab(draw, px(x, top + 176 * scale), px(x + 8 * scale, top + 189 * scale), c, c);
        }
        draw->AddText(body_, 12.5f * scale, px(right - 42 * scale - wide.x, top + 176 * scale), ImGui::GetColorU32(tint), level);
    }

    const Screen screen = loading ? Screen::Loading : has_race ? Screen::Paused : Screen::Ready;
    const bool focus_primary = screen != screen_;
    screen_ = screen;
    Action action = Stay;
    const float row = top + 404.f * scale;
    const float half = (panel_width - 12 * scale) * 0.5f, third = (panel_width - 24 * scale) / 3;
    if (loading) {
        // Start lights fill one by one while the race is prepared.
        const ImVec2 a = px(left, top + 262 * scale), b = px(left + panel_width, top + 330 * scale);
        draw->AddRectFilled(a, b, rgba(0.03f, 0.05f, 0.09f, 0.85f), 8 * scale);
        const float cycle = std::fmod(time * 1.25f, 7.f);
        for (int i = 0; i < 5; ++i) {
            const float cx = a.x + panel_width * (i + 0.5f) / 5, cy = (a.y + b.y) * 0.5f;
            const bool lit = cycle >= i + 1 && cycle < 6;
            const bool go = cycle >= 6;
            const ImU32 lamp = go ? rgba(0.25f, 1, 0.45f) : lit ? rgba(1, 0.18f, 0.14f) : rgba(0.20f, 0.07f, 0.08f);
            if (lit || go) draw->AddCircleFilled({cx, cy}, 22 * scale, go ? rgba(0.25f, 1, 0.45f, 0.18f) : rgba(1, 0.2f, 0.1f, 0.2f));
            draw->AddCircleFilled({cx, cy}, 16 * scale, lamp);
            draw->AddCircleFilled({cx - 5 * scale, cy - 5 * scale}, 4 * scale, rgba(1, 1, 1, lit || go ? 0.45f : 0.08f));
        }
        if (focus_primary) ImGui::SetKeyboardFocusHere();
        if (button(TX("Back"), left, top + 346 * scale, panel_width, 46 * scale, Look::Plain, 17)) action = Cancel;
        ImGui::SetItemDefaultFocus();
    } else {
        text(body_, 12.5f, left, top + 256 * scale, muted, has_race ? TX("Mode for the next race") : TX("Game mode"));
        if (button(TX("Normal race"), left, top + 274 * scale, half, 38 * scale,
                   mode_ == rt::RaceMode::Normal ? Look::Chosen : Look::Plain))
            mode_ = rt::RaceMode::Normal;
        if (button(TX("Time Attack"), left + half + 12 * scale, top + 274 * scale, half, 38 * scale,
                   mode_ == rt::RaceMode::TimeAttack ? Look::Chosen : Look::Plain))
            mode_ = rt::RaceMode::TimeAttack;
        text(body_, 12.5f, left, top + 326 * scale, muted, has_race ? TX("Gearbox for the next race") : TX("Gearbox"));
        if (button(TX("Automatic"), left, top + 344 * scale, half, 38 * scale,
                   transmission_ == rt::RaceTransmission::Automatic ? Look::Chosen : Look::Plain))
            transmission_ = rt::RaceTransmission::Automatic;
        if (button(TX("Manual"), left + half + 12 * scale, top + 344 * scale, half, 38 * scale,
                   transmission_ == rt::RaceTransmission::Manual ? Look::Chosen : Look::Plain))
            transmission_ = rt::RaceTransmission::Manual;
        if (focus_primary) ImGui::SetKeyboardFocusHere();
        if (button(has_race ? TX("Resume") : TX("Play"), left, row, panel_width, 64 * scale, Look::Primary, 30))
            action = has_race ? Resume : Play;
        ImGui::SetItemDefaultFocus();
        const char *numbers[] = {"01", "02", "03"};
        for (unsigned i = 0; i < 3; ++i)
            if (button(numbers[i], left + i * (third + 12 * scale), row + 76 * scale, third, 50 * scale,
                       course == i ? Look::Chosen : Look::Plain, 21, levels[i], course == i ? course_tint[i] : ImVec4()))
                course_ = Course(i);
        if (button(TX("Settings"), left, row + 138 * scale, half, 44 * scale, Look::Plain)) action = Settings;
        if (button(TX("Quit"), left + half + 12 * scale, row + 138 * scale, half, 44 * scale, Look::Plain)) action = Quit;
        if (has_race && button(TX("New race"), left, row + 194 * scale, panel_width, 40 * scale, Look::Plain)) action = Play;
        // Online play through a daytona-relay server (app/online_menu.h)
        const float online = row + (has_race ? 246.f : 194.f) * scale;
        if (button(TX("Online"), left, online, panel_width, 40 * scale, Look::Plain)) action = Online;
        const float beat = 0.5f + 0.5f * std::sin(time * 2.4f);
        draw->AddCircleFilled(px(left + panel_width - 30 * scale, online + 20 * scale), 4.5f * scale,
                              rgba(0.30f, 1, 0.55f, 0.55f + 0.45f * beat));
    }
    if (message && *message) {
        const float y = loading ? top + 408 * scale : row + (has_race ? 298.f : 246.f) * scale;
        ImGui::SetCursorScreenPos(px(left + 12 * scale, y));
        ImGui::PushFont(body_, 15 * scale);
        ImGui::PushTextWrapPos(px(left + panel_width, 0).x);
        ImGui::TextColored({1, 0.74f, 0.42f, 1}, "%s", message);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        draw->AddRectFilled(px(left, y + 2 * scale), {px(left + 4 * scale, 0).x, ImGui::GetItemRectMax().y - 2 * scale}, orange);
    }
    // Bottom bar: a checkered run, the session and the display.
    const float bottom = h - 46 * scale;
    {
        // A dark floor under the bar keeps it readable over a bright scene.
        draw->AddRectFilledMultiColor(px(0, bottom - 34 * scale), px(w, h), rgba(0.02f, 0.04f, 0.09f, 0),
                                      rgba(0.02f, 0.04f, 0.09f, 0), rgba(0.02f, 0.04f, 0.09f, 0.62f),
                                      rgba(0.02f, 0.04f, 0.09f, 0.62f));
        const float cell = 5 * scale;
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 2; ++j)
                if ((i + j) % 2 == 0)
                    draw->AddRectFilled(px(left + i * cell, bottom - 1 * scale + j * cell),
                                        px(left + (i + 1) * cell, bottom - 1 * scale + (j + 1) * cell), rgba(1, 1, 1, 0.55f));
        draw->AddRectFilledMultiColor(px(left + 24 * cell + 8 * scale, bottom + 4 * scale), px(w - left, bottom + 5 * scale),
                                      rgba(1, 1, 1, 0.35f), rgba(1, 1, 1, 0.05f), rgba(1, 1, 1, 0.05f), rgba(1, 1, 1, 0.35f));
        text(body_, 12, left, bottom + 16 * scale, muted, TX("SOLO   /   FREE PLAY"));
        char display[80];
        const auto ratio = ImGui::GetIO().DisplayFramebufferScale;
        std::snprintf(display, sizeof display, TX("SDR   /   %d × %d"), int(w * ratio.x), int(h * ratio.y));
        const ImVec2 wide = measure(body_, 12 * scale, display);
        draw->AddText(body_, 12 * scale, px(w - left - wide.x, bottom + 16 * scale), muted, display);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return action;
}

} // namespace app
