#pragma once

// Online play (app/relay_link.h): the cabinet's own settings for a linked
// race, written before the first frame as PcSolo::configure does; then, once
// a frame, one Start and one course for the whole room (AutoEntry,
// StartGate, CourseSync, below). The relay
// server numbers the players: the first in the room is the MASTER (car 1),
// the others SLAVEs (car 2, 3...). Every player gets the server's race
// settings, so the linked cabinets always agree (the game cancels a link
// whose cabinets differ). Revision A's settings EEPROM (test menu > GAME
// SYSTEM; mapped through the test menu, 23/11):
//
//   0x08-0x09 CRC-16/XMODEM of 0x0a..0x7f, little endian
//   0x0b LINK ID      00 SINGLE, 01 MASTER, 02 SLAVE
//   0x0c CAR NUMBER   car - 1
//   0x10 coin assignment (26: FREE PLAY), 0x14 free-play flag (pc_solo.h)
//   0x19 ADVERTISE SOUND 00 ON, 01 OFF
//   0x1a CABINET      01 TWIN, 02 UPLIGHT, 00 DELUXE
//   0x1b COUNTRY      01 JPN, 00 USA, 02 EXPORT
//   0x1c GAME MODE    00 NORMAL, 01 GRAND PRIX, 02 ENDURANCE
//   0x1d RIVAL ARROW  00 ON, 01 OFF
//   0x20 DIFFICULTY   00 EASY, 01 NORMAL, 02 HARD, 03 HARDEST

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rt::online {

struct Setting {
    uint8_t offset, value;
};

// The bytes a server may set: the race settings above, nothing else.
inline bool allowed(uint8_t offset) {
    switch (offset) {
    case 0x10: case 0x14: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x20: return true;
    default: return false;
    }
}

inline uint16_t settings_crc(const std::array<uint8_t, 128> &settings) {
    uint16_t value = 0;
    for (size_t i = 10; i < settings.size(); ++i) {
        value ^= uint16_t(unsigned(settings[i]) << 8);
        for (int bit = 0; bit < 8; ++bit)
            value = uint16_t((unsigned(value) << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
    }
    return value;
}

// position: this player's place in the room, 1..8 (1 = MASTER).
template <class Board> void configure(Board &board, int position, const std::vector<Setting> &race) {
    auto &settings = board.io().eeprom;
    constexpr uint32_t factory = 0x239d60; // the program's factory settings (pc_solo.h)
    bool valid = true;
    for (unsigned i = 0; i < 8; ++i) valid &= settings[i] == board.read_byte(factory + i);
    valid &= (unsigned(settings[8]) | (unsigned(settings[9]) << 8)) == settings_crc(settings);
    if (!valid)
        for (unsigned i = 0; i < settings.size(); ++i) settings[i] = board.read_byte(factory + i);
    position = std::clamp(position, 1, 8);
    settings[0x0b] = position == 1 ? 1 : 2;   // LINK ID MASTER / SLAVE
    settings[0x0c] = uint8_t(position - 1);   // CAR NUMBER
    for (const auto &s : race)
        if (allowed(s.offset)) settings[s.offset] = s.value;
    const auto checksum = settings_crc(settings);
    settings[8] = uint8_t(checksum);
    settings[9] = uint8_t(checksum >> 8);
    // the guest keeps two copies in backup RAM, as PcSolo::configure writes
    auto &backup = board.backup_ram();
    std::copy(settings.begin(), settings.end(), backup.begin());
    std::copy(settings.begin(), settings.end(), backup.begin() + settings.size());
}

// Online: one Start for the whole room. When another linked cabinet starts a
// race, this one shows WAITING FOR YOUR ENTRY (the mode dispatch at 0x5010a0,
// as pc_solo.h reads it: 16 while the entry countdown runs, about 16 s,
// observed 23/11 on two linked cabinets) and joins only if its own Start is
// pressed. AutoEntry presses it: short edges until the guest leaves the
// state, as PcSolo presents its Start. Board inputs only, no RAM writes.
class AutoEntry {
public:
    static constexpr uint32_t mode_address = 0x5010a0;
    static constexpr int waiting_for_entry = 16;
    template <class Board, class Inputs> void apply(Board &board, Inputs &inputs) {
        if (board.read_byte(mode_address) != waiting_for_entry) {
            age_ = 0;
            return;
        }
        ++age_;
        if (age_ >= 20 && (age_ - 20) % 24 < 5) inputs.in0 &= 0xef; // Start
    }

private:
    unsigned age_ = 0;
};

// Online: only the MASTER starts a race (unless the server lets anyone): a
// SLAVE's Start is released while its guest is in attract (modes below the
// entry countdown), so it can still join, select and race.
struct StartGate {
    template <class Board, class Inputs> static void apply(Board &board, Inputs &inputs, bool may_start) {
        if (!may_start && board.read_byte(AutoEntry::mode_address) < 15) inputs.in0 |= 0x10;
    }
};

// Online: the MASTER's course for the whole room. Linked cabinets each choose
// their own course, and the guest settles a disagreement by itself (observed
// 23/11: MASTER Expert, SLAVE Beginner -> both race Beginner). So the MASTER
// tells the room where its cursor is and what it confirmed (the selection
// step at 0x5010f8 leaves 1 on confirmation; the course at 0x501460, guest
// order 0 short, 2 medium, 1 long, live while step 1 runs, pc_solo.h), and
// each SLAVE's steering follows it, its pedal confirming once the MASTER has.
// If the SLAVE's own timer ends first, its cursor is already the MASTER's.
// The gearbox stays each driver's own. Board inputs only.
class CourseSync {
public:
    static constexpr uint32_t step_address = 0x5010f8, course_address = 0x501460;
    static constexpr int selection = 18, course_step = 1, race = 22;

    // MASTER, once a frame: "cursor=N" when its cursor moves, "course=N" when
    // it confirms, else empty.
    template <class Board> std::string master(Board &board) {
        const int mode = board.read_byte(AutoEntry::mode_address), step = board.read_byte(step_address);
        std::string out;
        if (mode == selection && step == course_step) {
            const int course = board.read_byte(course_address);
            if (course != sent_) out = "cursor=" + std::to_string(sent_ = course);
        } else if (mode == selection && last_step_ == course_step) {
            out = "course=" + std::to_string(board.read_byte(course_address));
        }
        if (mode != selection) sent_ = -1;
        last_step_ = mode == selection ? step : -1;
        return out;
    }

    // SLAVE: a message from the MASTER.
    void receive(const std::string &message) {
        int value = -1;
        if (std::sscanf(message.c_str(), "cursor=%d", &value) == 1 && value >= 0 && value <= 2) cursor_ = value;
        else if (std::sscanf(message.c_str(), "course=%d", &value) == 1 && value >= 0 && value <= 2)
            cursor_ = value, confirmed_ = true;
    }

    // SLAVE, once a frame: its course inputs follow the MASTER's.
    template <class Board, class Inputs> void slave(Board &board, Inputs &inputs) {
        const int mode = board.read_byte(AutoEntry::mode_address);
        if (mode < 15 || mode >= race) { // attract or racing: the next selection starts afresh
            cursor_ = -1;
            confirmed_ = false;
            age_ = 0;
            return;
        }
        if (mode != selection || board.read_byte(step_address) != course_step) return;
        constexpr uint8_t steer[] = {0x80, 0xe0, 0xb0}; // guest order: short, long, medium (pc_solo.h)
        inputs.steer = steer[cursor_ >= 0 ? cursor_ : 0];
        inputs.accel = 0x20; // the player's pedal does not choose
        if (confirmed_ && ++age_ >= 30 && (age_ - 30) % 24 < 5) inputs.accel = 0xe0;
    }

    int cursor() const { return cursor_; }
    bool confirmed() const { return confirmed_; }

private:
    int sent_ = -1, last_step_ = -1;           // MASTER
    int cursor_ = -1;                          // SLAVE
    bool confirmed_ = false;
    unsigned age_ = 0;
};

// Online: the linked race's standings for the players list. Each cabinet
// keeps its own place among the linked players at 0x501908 (0 = first; it
// matched every cabinet's POSITION on five linked cabinets, 23/11; a mirror
// at 0x5051ed; not the 40-car place of a solo race). Each game tells the
// room its own ("pos=CAR:RANK", on a change and every 2 s) and keeps the
// others'. Cleared outside a race.
class Standings {
public:
    static constexpr uint32_t rank_address = 0x501908;

    // Once a frame: this cabinet's place; a message for the room, or empty.
    template <class Board> std::string own(Board &board, int car) {
        if (board.read_byte(AutoEntry::mode_address) != CourseSync::race) {
            clear();
            return {};
        }
        const int rank = board.read_byte(rank_address);
        if (car < 1 || car > 8 || rank > 7) return {};
        rank_[car] = rank;
        if (rank == sent_ && ++since_ < 115) return {};
        sent_ = rank;
        since_ = 0;
        return "pos=" + std::to_string(car) + ":" + std::to_string(rank);
    }

    // A message from another player; false if it is not one of ours.
    bool receive(const std::string &message) {
        int car = 0, rank = 0;
        if (std::sscanf(message.c_str(), "pos=%d:%d", &car, &rank) != 2) return false;
        if (car >= 1 && car <= 8 && rank >= 0 && rank <= 7) rank_[car] = rank;
        return true;
    }

    int rank(int car) const { return car >= 1 && car <= 8 ? rank_[car] : -1; } // -1: not known
    void clear() {
        rank_.fill(-1);
        sent_ = -1;
        since_ = 0;
    }

private:
    std::array<int, 9> rank_{-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int sent_ = -1;
    unsigned since_ = 0;
};

} // namespace rt::online
