#include "app/online_overlay.h"
#include "app/i18n.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

namespace app {

uint32_t online_car_colour(int car) {
    // CAR NUMBER 1..8, the lit paint of each linked car (measured on an
    // 8-cabinet race through the relay): red, blue, yellow, green, charcoal,
    // pink, cyan, orange. 0.12.0: the first measurement read m2run's BGRA
    // dumps as RGB, red and blue swapped (violet for the red car 1)
    static const uint32_t colours[] = {0xd84870, 0x1c8aff, 0xf0e82c, 0x2ad89a,
                                       0x44403c, 0xf0c4d8, 0x1ee2f2, 0xf09a5a};
    return car >= 1 && car <= 8 ? colours[car - 1] : 0x808080;
}

namespace {
// Lean everything drawn since vertex `from` to the right, like the game's
// italic HUD figures: x moves by (pivot - y) * lean.
void lean(ImDrawList *draw, int from, float pivot, float lean) {
    for (int i = from; i < draw->VtxBuffer.Size; ++i) draw->VtxBuffer[i].pos.x += (pivot - draw->VtxBuffer[i].pos.y) * lean;
}

// Text with a black outline, as the arcade HUD draws its figures.
void outlined(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, ImU32 outline, float width,
              const char *text) {
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy) draw->AddText(font, size, {at.x + dx * width, at.y + dy * width}, outline, text);
    draw->AddText(font, size, at, colour, text);
    draw->AddText(font, size, {at.x + width * 0.5f, at.y}, colour, text); // a little bolder
}

// The colour a car's name is written in: its paint, the charcoal car's lightened
// to read on the dark bars.
uint32_t name_colour(int car) { return car == 5 ? 0xb6b0a8 : online_car_colour(car); }

// Dark car colours (5 charcoal, 1 red, 2 blue) take white figures, the others black.
bool light(uint32_t rgb) {
    const int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return r * 299 + g * 587 + b * 114 > 140000;
}
} // namespace

void draw_online_players(const online::Status &room, float alpha, const std::array<int, 9> &ranks) {
    if (alpha <= 0.f || room.names.empty()) return;
    const auto *view = ImGui::GetMainViewport();
    const float s = std::max(0.5f, view->Size.y / 768.f);
    auto *draw = ImGui::GetForegroundDrawList();
    const auto rgba = [alpha](uint32_t rgb, float a = 1.f) {
        return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(255 * a * alpha));
    };
    ImFont *font = ImGui::GetFont();
    constexpr float kLean = 0.22f;                   // the HUD's italic
    constexpr uint32_t kYellow = 0xffd400, kLime = 0x6cff2a, kWhite = 0xffffff, kBlack = 0x000000;
    const float title_size = 24.f * s, name_size = 22.f * s, figure_size = 26.f * s;
    const float row_h = 34.f * s, gap = 5.f * s, tab_w = 46.f * s, stripe = 7.f * s;
    const ImVec2 at(view->Pos.x + 22.f * s, view->Pos.y + view->Size.y * 0.19f);

    // the order: by place in the race when every player's is known, else by car
    const int players = std::min(int(room.names.size()), 8);
    std::vector<int> cars;
    bool standings = players > 1;
    for (int car = 1; car <= players; ++car) {
        cars.push_back(car);
        standings &= ranks[car] >= 0;
    }
    if (standings) std::stable_sort(cars.begin(), cars.end(), [&](int a, int b) { return ranks[a] < ranks[b]; });

    // the names first, to size the bars to the longest
    std::vector<std::string> names;
    float name_w = 0.f;
    for (int car : cars) {
        char unnamed[64];
        std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), car);
        std::string name = room.names[car - 1].empty() ? unnamed : room.names[car - 1];
        if (car == room.position) name += TX(" (you)");
        name_w = std::max(name_w, font->CalcTextSizeA(name_size, FLT_MAX, 0.f, name.c_str()).x);
        names.push_back(std::move(name));
    }
    const float bar_w = tab_w + stripe + 14.f * s + std::max(name_w, 150.f * s) + 26.f * s;

    // title, leaning, yellow on black, over a strip of chequers
    {
        const char *title = standings ? TX("STANDINGS") : TX("PLAYERS");
        const int from = draw->VtxBuffer.Size;
        outlined(draw, font, title_size, {at.x + 4.f * s, at.y}, rgba(kYellow), rgba(kBlack), 2.f * s, title);
        const float cy = at.y + title_size + 3.f * s, cell = 6.f * s;
        for (int col = 0; col < int(bar_w * 0.6f / cell); ++col)
            for (int row = 0; row < 2; ++row)
                draw->AddRectFilled({at.x + col * cell, cy + row * cell}, {at.x + (col + 1) * cell, cy + (row + 1) * cell},
                                    rgba((col + row) % 2 ? kWhite : kBlack, 0.85f));
        lean(draw, from, cy + 2 * cell, kLean);
    }

    float y = at.y + title_size + 3.f * s + 12.f * s + 8.f * s;
    for (size_t i = 0; i < cars.size(); ++i, y += row_h + gap) {
        const int car = cars[i];
        const bool me = car == room.position;
        const uint32_t colour = online_car_colour(car);
        const int from = draw->VtxBuffer.Size;
        // the bar: dark fading to the right, the player's own edged in yellow
        draw->AddRectFilledMultiColor({at.x, y}, {at.x + bar_w, y + row_h}, rgba(kBlack, 0.85f), rgba(kBlack, 0.5f),
                                      rgba(kBlack, 0.5f), rgba(kBlack, 0.85f));
        if (me) draw->AddRect({at.x, y}, {at.x + bar_w, y + row_h}, rgba(kYellow), 0.f, 0, 2.5f * s);
        // the tab: the place in the race (lime, as the HUD's POSITION) on black,
        // or the car's number on its colour
        char figure[16];
        if (standings) {
            std::snprintf(figure, sizeof figure, TX("%d."), ranks[car] + 1);
            draw->AddRectFilled({at.x, y}, {at.x + tab_w, y + row_h}, rgba(kBlack, 0.9f));
            draw->AddRectFilled({at.x + tab_w, y}, {at.x + tab_w + stripe, y + row_h}, rgba(colour));
        } else {
            std::snprintf(figure, sizeof figure, "%d", car);
            draw->AddRectFilled({at.x, y}, {at.x + tab_w, y + row_h}, rgba(colour));
            draw->AddRectFilled({at.x + tab_w, y}, {at.x + tab_w + stripe, y + row_h}, rgba(kBlack, 0.9f));
        }
        const ImVec2 fs = font->CalcTextSizeA(figure_size, FLT_MAX, 0.f, figure);
        const ImVec2 fat(at.x + (tab_w - fs.x) * 0.5f, y + (row_h - fs.y) * 0.5f);
        if (standings) outlined(draw, font, figure_size, fat, rgba(kLime), rgba(kBlack), 2.f * s, figure);
        else outlined(draw, font, figure_size, fat, rgba(light(colour) ? kBlack : kWhite),
                      rgba(light(colour) ? kWhite : kBlack, 0.6f), 1.f * s, figure);
        // the name in its car's colour, outlined (the player's own row is edged in yellow)
        const ImVec2 ns = font->CalcTextSizeA(name_size, FLT_MAX, 0.f, names[i].c_str());
        outlined(draw, font, name_size, {at.x + tab_w + stripe + 14.f * s, y + (row_h - ns.y) * 0.5f},
                 rgba(name_colour(car)), rgba(kBlack), 2.f * s, names[i].c_str());
        lean(draw, from, y + row_h, kLean);
    }
}

void CarTagOverlay::update(const std::vector<rt::online::CarTag> &tags, float seconds) {
    std::array<bool, 9> drawn{};
    for (const auto &tag : tags) {
        if (tag.car < 1 || tag.car > 8) continue;
        tags_[tag.car] = tag;
        drawn[tag.car] = true;
    }
    for (int car = 1; car <= 8; ++car) {
        float &a = shown_[car];
        if (!drawn[car]) a = 0.f; // off the screen, behind the camera, too far: nowhere to put it
        else if (tags_[car].visible > 0.f) a = std::min(1.f, a + seconds * 6.f);
        else a = std::max(0.f, a - seconds * 4.f); // hidden by the scenery: fades where it is
    }
}

void CarTagOverlay::draw(const online::Status &room, Picture picture, int screen_w, int screen_h, float alpha) const {
    if (alpha <= 0.f || screen_w <= 0 || screen_h <= 0 || picture.w <= 0.f) return;
    const auto *view = ImGui::GetMainViewport();
    auto *draw = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    constexpr float kLean = 0.22f; // the HUD's italic, as the list
    constexpr uint32_t kWhite = 0xffffff, kBlack = 0x000000;
    const float left = view->Pos.x + picture.x, top = view->Pos.y + picture.y;
    const float sx = picture.w / float(screen_w), sy = picture.h / float(screen_h);

    std::vector<int> cars; // far first, so a nearer tag is drawn over a farther one
    for (int car = 1; car <= 8; ++car)
        if (shown_[car] > 0.f && car != room.position) cars.push_back(car);
    std::sort(cars.begin(), cars.end(), [&](int a, int b) { return tags_[a].depth > tags_[b].depth; });

    for (int car : cars) {
        const auto &tag = tags_[car];
        const float a = alpha * shown_[car] * distance_alpha(tag.depth);
        if (a < 0.01f) continue;
        const auto rgba = [a](uint32_t rgb, float k = 1.f) {
            return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(255 * a * k));
        };
        const float x = left + tag.x * sx, y = top + tag.top * sy;
        if (x < left || x > left + picture.w || tag.bottom * sy < 0.f || y > top + picture.h) continue;
        const float s = std::max(0.4f, picture.h / 768.f) * distance_scale(tag.depth);
        const uint32_t colour = online_car_colour(car);

        char unnamed[64];
        std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), car);
        const char *name = size_t(car) <= room.names.size() && !room.names[car - 1].empty()
                               ? room.names[car - 1].c_str() : unnamed;
        char figure[4];
        std::snprintf(figure, sizeof figure, "%d", car);
        const float name_size = 19.f * s, figure_size = 19.f * s;
        const float h = 25.f * s, tab_w = 24.f * s, pad = 8.f * s, point = 8.f * s;
        const float name_w = font->CalcTextSizeA(name_size, FLT_MAX, 0.f, name).x;
        const float bar_w = tab_w + pad + name_w + pad;
        // the pointer's tip just over the roof; the bar over it, kept inside the picture
        const float tip = std::max(y - 3.f * s, top + h + point);
        const float bottom = tip - point;
        const float bar_x = std::clamp(x - bar_w * 0.5f, left, std::max(left, left + picture.w - bar_w));

        draw->AddTriangleFilled({x - point * 0.75f, bottom - 1.f}, {x + point * 0.75f, bottom - 1.f}, {x, tip},
                                rgba(colour));
        draw->AddTriangle({x - point * 0.75f, bottom - 1.f}, {x + point * 0.75f, bottom - 1.f}, {x, tip},
                          rgba(kBlack, 0.8f), 1.5f * s);
        const int from = draw->VtxBuffer.Size;
        draw->AddRectFilledMultiColor({bar_x, bottom - h}, {bar_x + bar_w, bottom}, rgba(kBlack, 0.85f),
                                      rgba(kBlack, 0.6f), rgba(kBlack, 0.6f), rgba(kBlack, 0.85f));
        draw->AddRectFilled({bar_x, bottom - h}, {bar_x + tab_w, bottom}, rgba(colour));
        draw->AddRectFilled({bar_x, bottom - 2.f * s}, {bar_x + bar_w, bottom}, rgba(colour));
        const ImVec2 fs = font->CalcTextSizeA(figure_size, FLT_MAX, 0.f, figure);
        outlined(draw, font, figure_size, {bar_x + (tab_w - fs.x) * 0.5f, bottom - h + (h - fs.y) * 0.5f - 1.f * s},
                 rgba(light(colour) ? kBlack : kWhite), rgba(light(colour) ? kWhite : kBlack, 0.6f), 1.f * s, figure);
        const float ny = bottom - h + (h - name_size) * 0.5f - 1.f * s;
        outlined(draw, font, name_size, {bar_x + tab_w + pad, ny}, rgba(name_colour(car)), rgba(kBlack), 1.5f * s,
                 name); // in its car's colour, as in the players list
        lean(draw, from, bottom, kLean);
    }
}

} // namespace app
