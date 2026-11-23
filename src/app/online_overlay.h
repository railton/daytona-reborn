// Online play: the room's players over the race (top left), each with the
// number and colour of their car, as the course map's markers show them.
#pragma once

#include "app/online.h"

#include <cstdint>

namespace app {

// The colour of a linked cabinet's car, by CAR NUMBER (1..8), as RGB; what
// the game paints them, seen on eight linked cabinets (23/11).
uint32_t online_car_colour(int car);

// The player list, faded by alpha (0..1). room.position is this player's
// place (and car number, rt::online::configure).
void draw_online_players(const online::Status &room, float alpha);

} // namespace app
