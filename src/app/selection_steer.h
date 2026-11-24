// The cabinet's course screen and gearbox screen choose by where the wheel
// is held. Measured with m2run steering sweeps (Revision A):
//
//   mode 0x5010a0 = 18 (selection), step 0x5010f8 = 1: the course
//     steering <= 0x85 Beginner, 0x86..0xc5 Advanced, >= 0xc6 Expert
//     (course byte 0x501460: 0, 2, 1); left of centre is Beginner
//   same mode, step 7 (a cabinet alone) or step 5 (linked cabinets): the gearbox
//     steering <= 0x97 first option, >= 0x98 second (0x501464: 0, 1)
//
// Keys, a gamepad stick or touch spring back to the centre (0x80), so the
// cursor always fell back to the first option. For them, each push of the
// steering past half lock moves a held cursor one option, and the steering
// sent to the cabinet stays on that option until the screen ends. A wheel
// keeps its own position. Inputs only: no RAM is written.
#pragma once

#include <algorithm>
#include <cstdint>

namespace app {

class SelectionSteer {
public:
    static constexpr uint32_t mode_address = 0x5010a0, step_address = 0x5010f8;
    static constexpr int selection = 18, course_step = 1, gearbox_step = 7, linked_gearbox_step = 5;
    static constexpr uint8_t course_stops[] = {0x80, 0xa6, 0xe0}; // Beginner, Advanced, Expert
    static constexpr uint8_t gearbox_stops[] = {0x80, 0xd0};

    // steer: this frame's steering (ADC, 0x20..0xe0), replaced on those
    // screens unless wheel_holds (an analogue wheel with no steering key).
    // linked: cabinets linked together (online, link play), whose gearbox
    // screen is step 5 (measured with m2run --relay, M2_TEST_SELECTION_TRACE).
    template <class Board> void apply(Board &board, uint8_t &steer, bool wheel_holds, bool linked = false) {
        const int mode = board.read_byte(mode_address), step = board.read_byte(step_address);
        const uint8_t *stops = nullptr;
        int count = 0;
        if (mode == selection && step == course_step) stops = course_stops, count = 3;
        else if (mode == selection && step == (linked ? linked_gearbox_step : gearbox_step)) stops = gearbox_stops, count = 2;
        const int screen = stops ? step : -1;
        if (screen != screen_) { // a new screen starts on its first option, as the centred wheel does
            option_ = 0;
            side_ = 0;
            screen_ = screen;
        }
        if (!stops || wheel_holds) return;
        // a push: past half lock; let go: back within a quarter of the centre (a stick hovering between keeps its side)
        const int s = steer;
        const int side = s >= 0xb0 ? 1 : s <= 0x50 ? -1 : (s > 0x68 && s < 0x98) ? 0 : side_;
        if (side && side != side_) option_ = std::clamp(option_ + side, 0, count - 1);
        side_ = side;
        steer = stops[option_];
    }

private:
    int screen_ = -1, option_ = 0, side_ = 0;
};

} // namespace app
