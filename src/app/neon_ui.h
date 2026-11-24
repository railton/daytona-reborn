// The home screen's look, shared by its screens (app/home_menu.cpp,
// app/online_menu.cpp): night-blue panels with neon edges, italic type,
// choices that light up, a yellow call to action. Drawn with ImGui's draw
// list; every size is in units of `scale` (the screen's height / 900).
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace app::neon {

inline ImU32 rgba(float r, float g, float b, float a = 1) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }

inline ImVec2 measure(ImFont *font, float size, const char *text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text); }

// Shrinks a size so the text fits a width (long translations).
inline float fit(ImFont *font, float size, const char *text, float width) {
    const float wide = measure(font, size, text).x;
    return wide > width ? size * width / wide : size;
}

// Text sheared into italics, optionally shaded top to bottom.
inline void italic(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 top, const char *text, float shear = 0.2f,
                   ImU32 bottom = 0) {
    const int first = draw->VtxBuffer.Size;
    draw->AddText(font, size, at, top, text);
    const int last = draw->VtxBuffer.Size;
    const float base = at.y + size * 0.8f;
    for (int i = first; i < last; ++i) draw->VtxBuffer[i].pos.x += (base - draw->VtxBuffer[i].pos.y) * shear;
    if (bottom) ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, last, at, {at.x, at.y + size}, top, bottom);
}

// A box leaning by k, its corners rounded by r: a closed outline.
inline std::vector<ImVec2> rounded(ImVec2 a, ImVec2 b, float k, float r) {
    const ImVec2 c[4] = {{a.x + k, a.y}, {b.x, a.y}, {b.x - k, b.y}, {a.x, b.y}};
    std::vector<ImVec2> out;
    auto toward = [r](ImVec2 from, ImVec2 to) {
        const float dx = to.x - from.x, dy = to.y - from.y, len = std::sqrt(dx * dx + dy * dy);
        const float t = len > 0 ? std::min(r, len * 0.5f) / len : 0;
        return ImVec2(from.x + dx * t, from.y + dy * t);
    };
    for (int i = 0; i < 4; ++i) {
        const ImVec2 q = c[i], s = toward(q, c[(i + 3) % 4]), e = toward(q, c[(i + 1) % 4]);
        for (int j = 0; j <= 6; ++j) {
            const float t = j / 6.f, u = 1 - t;
            out.push_back({u * u * s.x + 2 * u * t * q.x + t * t * e.x, u * u * s.y + 2 * u * t * q.y + t * t * e.y});
        }
    }
    return out;
}

inline void fill(ImDrawList *draw, const std::vector<ImVec2> &shape, ImU32 top, ImU32 bottom, float y0, float y1) {
    const int first = draw->VtxBuffer.Size;
    draw->AddConvexPolyFilled(shape.data(), int(shape.size()), top);
    if (top != bottom)
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, {0, y0}, {0, y1}, top, bottom);
}

// A neon edge: soft wide strokes under a bright thin one.
inline void glow(ImDrawList *draw, const std::vector<ImVec2> &shape, ImVec4 c, float strength, float scale) {
    for (int i = 5; i >= 1; --i) {
        const float a = strength * 0.15f * (1.f - (i - 1) / 5.f);
        draw->AddPolyline(shape.data(), int(shape.size()), ImGui::GetColorU32({c.x, c.y, c.z, a}), ImDrawFlags_Closed,
                          (1.5f + i * 2.6f) * scale);
    }
    draw->AddPolyline(shape.data(), int(shape.size()),
                      ImGui::GetColorU32({std::min(1.f, c.x + 0.35f), std::min(1.f, c.y + 0.2f), 1, std::min(1.f, strength)}),
                      ImDrawFlags_Closed, 1.6f * scale);
}

inline const ImVec4 kBlue(0.25f, 0.70f, 1, 1);

inline void gear(ImDrawList *d, ImVec2 c, float s, ImU32 col, ImU32 hole) {
    for (int i = 0; i < 8; ++i) {
        const float a = i * IM_PI / 4;
        d->AddLine({c.x + std::cos(a) * s * 0.25f, c.y + std::sin(a) * s * 0.25f},
                   {c.x + std::cos(a) * s * 0.48f, c.y + std::sin(a) * s * 0.48f}, col, s * 0.2f);
    }
    d->AddCircleFilled(c, s * 0.34f, col, 24);
    d->AddCircleFilled(c, s * 0.14f, hole, 16);
}

inline void plus(ImDrawList *d, ImVec2 c, float s, ImU32 col, float t) {
    d->AddLine({c.x - s * 0.4f, c.y}, {c.x + s * 0.4f, c.y}, col, t);
    d->AddLine({c.x, c.y - s * 0.4f}, {c.x, c.y + s * 0.4f}, col, t);
}

// Interactive pieces: an invisible ImGui button (keyboard and gamepad
// navigation included) under what is drawn.
struct Hit {
    bool clicked = false, focused = false, hovered = false, held = false;
};

inline Hit hit(const char *id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    Hit h;
    h.clicked = ImGui::InvisibleButton(id, {std::max(1.f, b.x - a.x), std::max(1.f, b.y - a.y)}, ImGuiButtonFlags_EnableNav);
    h.focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    h.hovered = ImGui::IsItemHovered();
    h.held = ImGui::IsItemActive();
    return h;
}

inline void focus_ring(ImDrawList *draw, const Hit &h, const std::vector<ImVec2> &shape, float scale) {
    if (h.focused) draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.9f, 0.35f), ImDrawFlags_Closed, 2.5f * scale);
    else if (h.hovered) draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 1, 1, 0.5f), ImDrawFlags_Closed, 1.5f * scale);
}

// One choice of a row's: blue and lit when chosen.
inline bool choice(ImDrawList *draw, ImFont *font, float scale, const char *id, const char *label, ImVec2 a, ImVec2 b,
                   bool chosen, float text_size = 21.f) {
    const Hit h = hit(id, a, b);
    const auto shape = rounded(a, b, (b.y - a.y) * 0.2f, 5 * scale);
    const float lift = h.focused || h.hovered ? 1.f : 0.f;
    if (chosen) {
        fill(draw, shape, rgba(0.35f, 0.72f, 1), rgba(0.15f, 0.37f, 0.75f), a.y, b.y);
        glow(draw, shape, ImVec4(0.25f, 0.62f, 1, 1), 0.7f, scale);
    } else {
        fill(draw, shape, rgba(0.07f + 0.05f * lift, 0.12f + 0.06f * lift, 0.24f + 0.08f * lift, 0.92f), rgba(0.03f, 0.06f, 0.14f, 0.92f),
             a.y, b.y);
        draw->AddPolyline(shape.data(), int(shape.size()), rgba(0.6f, 0.75f, 1, 0.25f + 0.25f * lift), ImDrawFlags_Closed, 1.2f * scale);
    }
    focus_ring(draw, h, shape, scale);
    const float s = fit(font, text_size * scale, label, b.x - a.x - 22 * scale);
    const ImVec2 wide = measure(font, s, label);
    italic(draw, font, s, {(a.x + b.x - wide.x) * 0.5f, (a.y + b.y - s) * 0.5f}, rgba(0.97f, 0.98f, 1), label, 0.14f);
    return h.clicked;
}

// A blue button; icon: 0 none, 1 plus, 2 gear. With no label, the icon
// alone, centred (id names it).
inline bool blue_button(ImDrawList *draw, ImFont *font, float scale, const char *id, const char *label, ImVec2 a, ImVec2 b,
                        int icon, float text_size = 21.f) {
    const Hit h = hit(id, a, b);
    const auto shape = rounded(a, b, (b.y - a.y) * 0.2f, 8 * scale);
    const float lift = h.focused || h.hovered ? 1.f : 0.f;
    fill(draw, shape, rgba(0.30f + 0.06f * lift, 0.66f + 0.06f * lift, 1), rgba(0.08f, 0.32f, 0.82f), a.y, b.y);
    glow(draw, shape, kBlue, 0.6f + 0.3f * lift, scale);
    focus_ring(draw, h, shape, scale);
    const ImU32 white = rgba(1, 1, 1);
    const float mid = (a.y + b.y) * 0.5f;
    if (!label || !*label) {
        const ImVec2 c = {(a.x + b.x) * 0.5f, mid};
        if (icon == 1) plus(draw, c, 20 * scale, white, 3 * scale);
        else if (icon == 2) gear(draw, c, 24 * scale, white, rgba(0.16f, 0.46f, 0.92f));
        return h.clicked;
    }
    const float room = b.x - a.x - (icon ? 60 : 28) * scale;
    const float size = fit(font, text_size * scale, label, room);
    const ImVec2 wide = measure(font, size, label);
    const float shift = icon ? 30 * scale : 0;
    const float x0 = (a.x + b.x - wide.x - shift) * 0.5f;
    if (icon == 1) plus(draw, {x0 + 9 * scale, mid}, 18 * scale, white, 3 * scale);
    else if (icon == 2) gear(draw, {x0 + 9 * scale, mid}, 19 * scale, white, rgba(0.16f, 0.46f, 0.92f));
    italic(draw, font, size, {x0 + shift, mid - size * 0.55f}, white, label, 0.14f);
    return h.clicked;
}

// A dark button lit in a colour of its own (red for Leave).
inline bool outline_button(ImDrawList *draw, ImFont *font, float scale, const char *label, ImVec2 a, ImVec2 b, ImVec4 colour,
                           float text_size = 20.f) {
    const Hit h = hit(label, a, b);
    const auto shape = rounded(a, b, (b.y - a.y) * 0.2f, 8 * scale);
    const float lift = h.focused || h.hovered ? 1.f : 0.f;
    fill(draw, shape, rgba(0.08f + 0.05f * lift, 0.10f + 0.04f * lift, 0.20f + 0.06f * lift, 0.95f), rgba(0.04f, 0.05f, 0.12f, 0.95f),
         a.y, b.y);
    glow(draw, shape, colour, 0.55f + 0.4f * lift, scale);
    focus_ring(draw, h, shape, scale);
    const float s = fit(font, text_size * scale, label, b.x - a.x - 24 * scale);
    const ImVec2 wide = measure(font, s, label);
    italic(draw, font, s, {(a.x + b.x - wide.x) * 0.5f, (a.y + b.y - s) * 0.5f}, rgba(0.97f, 0.98f, 1), label, 0.14f);
    return h.clicked;
}

// Signal bars for a latency: three lit up to 60 ms, two up to 120, one above.
inline void signal(ImDrawList *draw, ImVec2 at, float scale, int ms) {
    const int lit = ms < 0 ? 0 : ms <= 60 ? 3 : ms <= 120 ? 2 : 1;
    const ImU32 on = ms < 0 ? 0 : ms <= 60 ? rgba(0.25f, 0.88f, 0.48f) : ms <= 120 ? rgba(1, 0.80f, 0.28f) : rgba(1, 0.42f, 0.38f);
    for (int i = 0; i < 3; ++i) {
        const float x = at.x + i * 7 * scale, h = (6 + i * 5) * scale;
        draw->AddRectFilled({x, at.y - h}, {x + 4.5f * scale, at.y}, i < lit ? on : rgba(1, 1, 1, 0.15f), 1 * scale);
    }
}

// A text field in a dark, lit frame. Returns whether its text changed.
inline bool field(ImDrawList *draw, ImFont *font, float scale, const char *id, char *buf, size_t size, ImVec2 a, ImVec2 b,
                  ImGuiInputTextFlags flags = 0) {
    const auto shape = rounded(a, b, 0, 6 * scale);
    fill(draw, shape, rgba(0.01f, 0.03f, 0.08f, 0.94f), rgba(0.02f, 0.05f, 0.12f, 0.94f), a.y, b.y);
    const float text = 20 * scale, height = b.y - a.y;
    ImGui::PushFont(font, text);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0u);
    ImGui::PushStyleColor(ImGuiCol_Text, rgba(0.97f, 0.98f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, (height - text) * 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::SetCursorScreenPos({a.x + 14 * scale, a.y});
    ImGui::SetNextItemWidth(b.x - a.x - 26 * scale);
    const bool changed = ImGui::InputText(id, buf, size, flags);
    const bool nav = ImGui::IsItemFocused() && GImGui->NavCursorVisible, active = ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    glow(draw, shape, kBlue, nav || active ? 1.f : 0.5f, scale);
    if (nav) draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.9f, 0.35f), ImDrawFlags_Closed, 2.f * scale);
    return changed;
}

// The call to action: yellow, a play sign, a checkered end; dark (and
// without the play sign) for one that only goes back.
inline bool main_button(ImDrawList *draw, ImFont *font, float scale, const char *label, ImVec2 a, ImVec2 b, bool go,
                        float text_size = 40.f) {
    const float time = float(ImGui::GetTime());
    const Hit h = hit(label, a, b);
    const float height = b.y - a.y, k = height * 0.2f;
    const auto shape = rounded(a, b, k, 10 * scale);
    const float lift = h.held ? 0.f : (h.focused || h.hovered) ? 1.f : 0.f;
    if (go) {
        const float pulse = 0.5f + 0.5f * std::sin(time * 3.2f);
        for (int i = 4; i >= 1; --i) // a warm glow
            draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.70f, 0.15f, (0.10f + 0.06f * pulse) * (1 - (i - 1) / 4.f)),
                              ImDrawFlags_Closed, (2 + i * 4.f) * scale);
        fill(draw, shape, rgba(1, 0.90f + 0.05f * lift, 0.36f + 0.1f * lift), rgba(1, 0.58f, 0.06f), a.y, b.y);
        ImGui::PushClipRect(a, b, true); // the checkered end, fading in toward the right
        const float cell = height / 4;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) {
                if ((i + j) % 2) continue;
                const float x = b.x - k - (8 - i) * cell, y = a.y + j * cell;
                const float shift = (height - (y - a.y)) * 0.2f;
                draw->AddQuadFilled({x + shift, y}, {x + cell + shift, y}, {x + cell + shift - cell * 0.2f, y + cell},
                                    {x + shift - cell * 0.2f, y + cell}, rgba(0.10f, 0.07f, 0.02f, 0.55f * i / 7.f));
            }
        ImGui::PopClipRect();
        draw->AddPolyline(shape.data(), int(shape.size()), rgba(1, 0.95f, 0.65f, 0.9f), ImDrawFlags_Closed, 2 * scale);
    } else {
        fill(draw, shape, rgba(0.08f + 0.05f * lift, 0.14f + 0.06f * lift, 0.28f + 0.08f * lift, 0.95f), rgba(0.04f, 0.07f, 0.16f, 0.95f),
             a.y, b.y);
        glow(draw, shape, kBlue, 0.5f, scale);
    }
    focus_ring(draw, h, shape, scale);
    const float size = fit(font, text_size * scale, label, b.x - a.x - (go ? 140 : 36) * scale); // room for the play sign
    const ImVec2 wide = measure(font, size, label);
    const float play = go ? 46 * scale : 0;
    const float x0 = (a.x + b.x - wide.x - play) * 0.5f, mid = (a.y + b.y) * 0.5f;
    const ImU32 col = go ? rgba(0.06f, 0.05f, 0.03f) : rgba(0.97f, 0.98f, 1);
    if (go) draw->AddTriangleFilled({x0, mid - 15 * scale}, {x0 + 24 * scale, mid}, {x0, mid + 15 * scale}, col);
    italic(draw, font, size, {x0 + play, mid - size * 0.55f}, col, label, 0.16f);
    return h.clicked;
}

} // namespace app::neon
