#pragma once

// Online play (app/relay_link.h): the cabinet's own settings for a linked
// race, written before the first frame as PcSolo::configure does, and the
// entry into a race another player started (AutoEntry, below). The relay
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

} // namespace rt::online
