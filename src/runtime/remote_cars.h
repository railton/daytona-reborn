// Online play: how far each linked player's data advanced each tick, by the
// car record that draws that player's car, for the smooth drawing of the
// other players' cars (Geo::smooth_remote_cars).
//
// The block received at comm RAM 0x21c0 holds one slot of 0x1c0 bytes per
// other player: slot k the car k + 1 places behind this cabinet in the ring
// (the relay composes them, app/online_race_stats.h), its byte 0x0a that
// player's tick counter. A ring place is a CAR NUMBER (place 1 is car 1, the
// master), and each car record says which CAR NUMBER drives it (car_tags.h,
// +0x94). A step of 0 is a tick with no new data (the game holds the car
// where it was), 2 or more ticks that came in at once (it jumps ahead).
// The game moves the car a tick later than its data lands in comm RAM
// (measured, m2run --relay: a backlog of 21 ticks landed in one tick, the
// car jumped 12 units the next), so each step is told a tick late.
#pragma once

#include "runtime/car_tags.h"
#include "runtime/mobile_identity.h"

#include <array>
#include <cstdint>

namespace rt::online {

class RemoteCarSteps {
public:
    static constexpr unsigned kSlots = MobileIdentity::slot_count;

    // After each tick of the race: position (1-based) and size of the room's
    // ring. steps()[record]: -1 for a record not driven by another player,
    // else its data's step the tick before (as the game moves its car).
    template <class Board> const std::array<int8_t, kSlots> &update(Board &board, int position, int size) {
        steps_ = landed_; // the previous tick's, which the game's poses show now
        landed_.fill(-1);
        if (position < 1 || size < 2 || size > 8) return reset(), steps_;
        // which record each CAR NUMBER drives (39 bytes a tick)
        std::array<int, 8> records;
        records.fill(-1);
        for (unsigned slot = 0; slot < kSlots; ++slot)
            if (const int car = linked_car(board, slot); car >= 1 && car <= 8 && records[car - 1] < 0) records[car - 1] = int(slot);
        const uint8_t *comm = board.comm_ram();
        for (int k = 0; k + 1 < size; ++k) {
            const int car = ((position - 1 - (k + 1)) % size + size) % size + 1;
            const int record = records[car - 1];
            const int tick = comm[0x21c0 + k * 0x1c0 + 0x0a];
            int step = 1;
            if (previous_[k] >= 0) step = (tick - previous_[k]) & 0xff;
            previous_[k] = tick;
            if (record >= 0 && car != position) landed_[unsigned(record)] = int8_t(step > 100 ? 100 : step);
        }
        return steps_;
    }
    void reset() {
        previous_.fill(-1);
        landed_.fill(-1);
    }
    const std::array<int8_t, kSlots> &steps() const { return steps_; }

private:
    std::array<int8_t, kSlots> steps_{}, landed_ = filled(-1);
    static std::array<int8_t, kSlots> filled(int8_t v) {
        std::array<int8_t, kSlots> a;
        a.fill(v);
        return a;
    }
    std::array<int, 7> previous_{-1, -1, -1, -1, -1, -1, -1};
};

} // namespace rt::online
