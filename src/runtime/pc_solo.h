#pragma once

#include "runtime/m2_board.h"
#include "runtime/race_request.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace rt {

// Desktop adapter for the validated Revision A ROM set. It uses the cabinet's
// free-play setting and normal Start and pedal signals at the board boundary.
// No host key events, guest instruction patches or writes to live gameplay RAM.
class PcSolo {
public:
    using Course = RaceCourse;
    enum class Status { Starting, Racing, WrongCourse, TimedOut, WrongMode, WrongTransmission };

    explicit PcSolo(Course course, RaceTransmission transmission = RaceTransmission::Automatic,
                    RaceMode mode = RaceMode::Normal)
        : course_(course), transmission_(transmission), mode_(mode) {}

    template <class Board> static void configure(Board &board) {
        auto &settings = board.io().eeprom;
        constexpr uint32_t factory = 0x239d60;
        bool valid = true;
        for (unsigned i = 0; i < 8; ++i) valid &= settings[i] == board.read_byte(factory + i);
        valid &= (unsigned(settings[8]) | (unsigned(settings[9]) << 8)) == crc(settings);
        if (!valid)
            for (unsigned i = 0; i < settings.size(); ++i) settings[i] = board.read_byte(factory + i);
        settings[0x0b] = 0;  // Link ID SINGLE
        settings[0x10] = 26; // coin assignment 27, FREE PLAY (zero-based)
        settings[0x14] = 1;  // free-play flag consumed by the guest's credit gate
        const auto checksum = crc(settings);
        settings[8] = uint8_t(checksum);
        settings[9] = uint8_t(checksum >> 8);
        auto &backup = board.backup_ram();
        std::copy(settings.begin(), settings.end(), backup.begin());
        std::copy(settings.begin(), settings.end(), backup.begin() + settings.size());
    }

    template <class Board> Inputs input(Board &board) {
        Inputs input{};
        const int mode = board.read_byte(mode_address);
        const int step = mode == selection ? board.read_byte(selection_step) : 0;
        if (mode != last_mode_ || step != last_step_) {
            last_mode_ = mode;
            last_step_ = step;
            step_age_ = 0;
        }
        ++step_age_;
        ++age_;
        // A state can be entered before its input gate is open. Present a
        // short edge until the guest acknowledges it by leaving that state.
        const bool confirm = step_age_ >= 30 && (step_age_ - 30) % 24 < 5;
        if (mode == attract && confirm) input.in0 &= 0xef; // Start
        if (mode == selection && step == course_input) {
            constexpr uint8_t steer[] = {0x80, 0xb0, 0xe0};
            input.steer = steer[unsigned(course_)];
            if (confirm) input.accel = 0xe0;
        }
        if (mode == selection && step == gearbox_input) {
            // Proven ordinary board-input stimulus; retain automatic by default.
            input.steer = transmission_ == RaceTransmission::Manual ? 0xe0 : 0x80;
            if (mode_ == RaceMode::TimeAttack) input.in0 &= 0xef; // hold Start during pedal confirmation
            if (confirm) input.accel = 0xe0;
        }
        return input;
    }

    template <class Board> Status status(Board &board) const {
        if (board.read_byte(mode_address) == race) {
            constexpr uint8_t guest_course[] = {0, 2, 1}; // arcade table order: short, long, medium
            if (board.read_byte(course_address) != guest_course[unsigned(course_)]) return Status::WrongCourse;
            const auto expected_mode = mode_ == RaceMode::TimeAttack ? 1u : 0u;
            if (board.read_dword(time_attack_address) != expected_mode) return Status::WrongMode;
            const auto expected_transmission = transmission_ == RaceTransmission::Manual ? 1u : 0u;
            return board.read_byte(transmission_address) == expected_transmission
                ? Status::Racing : Status::WrongTransmission;
        }
        return age_ >= 1500 ? Status::TimedOut : Status::Starting;
    }

    template <class Board> static bool in_attract(Board &board) { return board.read_byte(mode_address) == attract; }
    template <class Board> static bool in_race(Board &board) { return board.read_byte(mode_address) == race; }

private:
    static uint16_t crc(const std::array<uint8_t, 128> &settings) {
        uint16_t value = 0;
        for (size_t i = 10; i < settings.size(); ++i) {
            value ^= uint16_t(unsigned(settings[i]) << 8);
            for (int bit = 0; bit < 8; ++bit)
                value = uint16_t((unsigned(value) << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
        }
        return value;
    }

    // Mode dispatch at 0x1994 and selection sub-dispatch at 0x301c.
    static constexpr uint32_t mode_address = 0x5010a0, selection_step = 0x5010f8, course_address = 0x501460;
    static constexpr uint32_t time_attack_address = 0x501a80; // committed mode used by race dispatch
    static constexpr uint32_t transmission_address = 0x501464; // committed gearbox selection
    static constexpr int attract = 3, selection = 18, race = 22;
    static constexpr int course_input = 1, gearbox_input = 7;
    Course course_;
    RaceTransmission transmission_;
    RaceMode mode_;
    int last_mode_ = -1, last_step_ = -1;
    unsigned step_age_ = 0, age_ = 0;
};

} // namespace rt
