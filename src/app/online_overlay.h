// Online play: the room's players over the race (top left), each with the
// number and colour of their car, as the course map's markers show them.
#pragma once

#include "app/online.h"

#include <array>
#include <cstdint>

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

} // namespace app
