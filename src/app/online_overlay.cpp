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
    // CAR NUMBER 1..8: purple, orange, cyan, lime, black, white, yellow, blue
    static const uint32_t colours[] = {0x6e46e6, 0xff8c1e, 0x1ec8d2, 0x8cd21e,
                                       0x282828, 0xe6e1f5, 0xfadc28, 0x3c82f0};
    return car >= 1 && car <= 8 ? colours[car - 1] : 0x808080;
}

void draw_online_players(const online::Status &room, float alpha, const std::array<int, 9> &ranks) {
    if (alpha <= 0.f || room.names.empty()) return;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.5f, view->Size.y / 768.f);
    auto *draw = ImGui::GetForegroundDrawList();
    const auto rgba = [alpha](uint32_t rgb, float a = 1.f) {
        return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(255 * a * alpha));
    };
    ImFont *font = ImGui::GetFont();
    const float title_size = 15.f * scale, row_size = 20.f * scale;
    const float pad = 12.f * scale, row_h = 30.f * scale, swatch = 18.f * scale;
    const ImVec2 at(view->Pos.x + 18.f * scale, view->Pos.y + view->Size.y * 0.19f);

    // the order: by place in the race when every player's is known, else by car
    const int players = std::min(int(room.names.size()), 8);
    std::vector<int> cars;
    bool standings = players > 1;
    for (int car = 1; car <= players; ++car) {
        cars.push_back(car);
        standings &= ranks[car] >= 0;
    }
    if (standings) std::stable_sort(cars.begin(), cars.end(), [&](int a, int b) { return ranks[a] < ranks[b]; });
    const char *title = standings ? TX("CLASSEMENT") : TX("JOUEURS");

    // rows first, to size the panel to the longest name
    std::vector<std::string> rows;
    float width = font->CalcTextSizeA(title_size, FLT_MAX, 0.f, title).x;
    for (int car : cars) {
        char unnamed[64], place[16];
        std::snprintf(unnamed, sizeof unnamed, TX("Joueur %d"), car);
        std::snprintf(place, sizeof place, TX("%d."), ranks[car] + 1);
        std::string row = (standings ? std::string(place) : std::to_string(car)) + "  " +
                          (room.names[car - 1].empty() ? unnamed : room.names[car - 1]);
        if (car == room.position) row += TX(" (vous)");
        width = std::max(width, swatch + 10.f * scale + font->CalcTextSizeA(row_size, FLT_MAX, 0.f, row.c_str()).x);
        rows.push_back(std::move(row));
    }
    const ImVec2 size(width + 2 * pad, pad + title_size + 6.f * scale + row_h * float(rows.size()) + pad * 0.5f);
    draw->AddRectFilled(at, {at.x + size.x, at.y + size.y}, rgba(0x000000, 0.6f), 8.f * scale);
    draw->AddText(font, title_size, {at.x + pad, at.y + pad * 0.7f}, rgba(0xb4e6ff), title);
    for (int i = 0; i < int(rows.size()); ++i) {
        const int car = cars[i];
        const float y = at.y + pad + title_size + 6.f * scale + row_h * float(i);
        const ImVec2 box(at.x + pad, y + (row_h - swatch) * 0.5f - 2.f * scale);
        draw->AddRectFilled(box, {box.x + swatch, box.y + swatch}, rgba(online_car_colour(car)), 3.f * scale);
        draw->AddRect(box, {box.x + swatch, box.y + swatch}, rgba(0xffffff, 0.8f), 3.f * scale, 0, 1.5f * scale);
        const bool me = car == room.position;
        draw->AddText(font, row_size, {box.x + swatch + 10.f * scale, y}, rgba(me ? 0x78ffbe : 0xffffff), rows[i].c_str());
    }
}

} // namespace app
