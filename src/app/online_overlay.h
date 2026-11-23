// Online play: the room's players over the race (top left), each with the
// number and colour of their car, as the course map's markers show them;
// and a name tag over each other player's car.
#pragma once

#include "app/online.h"
#include "runtime/car_tags.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace app {

// The colour of a linked cabinet's car, by CAR NUMBER (1..8), as RGB; what
// the game paints them, seen on eight linked cabinets (23/11).
uint32_t online_car_colour(int car);

// The player list, faded by alpha (0..1). room.position is this player's
// place (and car number, rt::online::configure). ranks: each car's place in
// the race (index = car number, 0 = first, -1 not known); when every
// player's is known, the list is the standings, in race order.
void draw_online_players(const online::Status &room, float alpha, const std::array<int, 9> &ranks = {
                             -1, -1, -1, -1, -1, -1, -1, -1, -1});

// Where the game's picture is in a view_w x view_h window, as main.cpp
// blits it: screen_w x screen_h (widescreen included) scaled to fit and
// centred, or stretched to 4:3 in the "4:3" mode.
struct Picture {
    float x = 0, y = 0, w = 0, h = 0;
};
inline Picture game_picture(float view_w, float view_h, int screen_w, int screen_h, bool arcade_43) {
    Picture p;
    if (view_w <= 0 || view_h <= 0 || screen_w <= 0 || screen_h <= 0) return p;
    if (arcade_43) {
        p.w = std::min(view_w, view_h * 4.f / 3.f);
        p.h = std::min(view_h, view_w * 3.f / 4.f);
    } else {
        const float scale = std::min(view_w / float(screen_w), view_h / float(screen_h));
        p.w = screen_w * scale;
        p.h = screen_h * scale;
    }
    p.x = (view_w - p.w) * 0.5f;
    p.y = (view_h - p.h) * 0.5f;
    return p;
}

// The name tags over the other players' cars (rt::online::car_tags), each
// in its car's colour, smaller and fainter with distance; a car hidden by
// the scenery fades out, one off the screen or behind the camera has none.
class CarTagOverlay {
public:
    // Once a presented frame: this frame's tags, seconds since the last.
    void update(const std::vector<rt::online::CarTag> &tags, float seconds);
    void clear() { shown_.fill(0.f); }
    bool empty() const {
        return std::all_of(shown_.begin(), shown_.end(), [](float a) { return a <= 0.f; });
    }
    // picture: the game's screen in the window (game_picture), screen_w x
    // screen_h its pixels. alpha fades them all.
    void draw(const online::Status &room, Picture picture, int screen_w, int screen_h, float alpha = 1.f) const;

    // Opacity for a car this far (camera units: the own car is about 4.4 from
    // the chase camera; a car 60 away is about 8 pixels wide on the 496 x 384
    // screen): whole up to 60, none from 160.
    static float distance_alpha(float depth) { return std::clamp((160.f - depth) / 100.f, 0.f, 1.f); }
    // Size for a car this far: whole up to 15, 0.7 from 90.
    static float distance_scale(float depth) { return 1.f - 0.3f * std::clamp((depth - 15.f) / 75.f, 0.f, 1.f); }

private:
    std::array<rt::online::CarTag, 9> tags_{}; // by car number
    std::array<float, 9> shown_{};             // each one's fade, 0..1
};

} // namespace app
