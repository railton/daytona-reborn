// Host-side evidence for the selection probe. C++17 standard library only.
#pragma once

#include <cstdint>
#include <stdexcept>

namespace tools {

// rt::Inputs defaults are steer 0x80, accel 0x20, brake 0x20,
// IN0 0xff, IN1 0x8f, IN2 0xff. Script::at restores them every tick.
// Only Start (IN0 bit 0x10, active low) may differ in this probe.
inline void require_selection_ports(uint8_t in0, uint8_t in1, uint8_t in2) {
    constexpr uint8_t start = 0x10;
    if (in1 != 0x8f || in2 != 0xff || uint8_t(in0 | start) != 0xff)
        throw std::runtime_error("selection probe refuses non-default IN1, IN2, or non-Start IN0");
}

// Completed GameLoop frames advance once per probe tick.
inline void require_frame_step(uint64_t frame_before, uint64_t frame_after) {
    if (frame_after != frame_before + 1)
        throw std::runtime_error("frame_after is not frame_before+1");
}

// Counts ticks that actually apply the requested steer in mode 18 / step 7.
struct SelectionEvidence {
    static constexpr uint8_t kMode = 18;
    static constexpr uint8_t kStep = 7;
    uint64_t samples = 0;
    uint64_t entered_frame = 0;

    bool note(uint8_t mode_before, uint8_t step_before, uint8_t applied_steer, uint8_t requested) {
        if (entered_frame || mode_before != kMode || step_before != kStep || applied_steer != requested)
            return false;
        ++samples;
        return true;
    }

    bool observe_entry(uint8_t before, uint8_t after, uint64_t frame_after) {
        if (entered_frame || before == 22 || after != 22 || !frame_after) return false;
        entered_frame = frame_after;
        return true;
    }
    bool success() const { return entered_frame != 0 && samples > 0; }
};

} // namespace tools
