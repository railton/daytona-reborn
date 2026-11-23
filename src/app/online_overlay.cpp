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
    // 8-cabinet race through the relay): violet, orange, cyan, lime,
    // charcoal, lilac, yellow, light blue
    static const uint32_t colours[] = {0x7048d8, 0xff8a1c, 0x2ce8f0, 0x9ad82a,
                                       0x3c4044, 0xd8c4f0, 0xf2e21e, 0x5a9af0};
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
uint32_t name_colour(int car) { return car == 5 ? 0xa8b0b6 : online_car_colour(car); }

// Dark car colours (5 charcoal, 1 violet) take white figures, the others black.
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

} // namespace app
