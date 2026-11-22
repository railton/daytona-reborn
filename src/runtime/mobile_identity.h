#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rt {

inline constexpr bool is_opponent_body_part(uint32_t part) {
    return part == 0x8808u || part == 0x8c00u || part == 0x9000u || part == 0x93dcu;
}

inline constexpr uint32_t opponent_wheel_near = 0x882cu;
inline constexpr uint32_t opponent_wheel_mid = 0x8c24u;

inline constexpr uint32_t opponent_wheel_chassis_part(uint32_t part) {
    switch (part) {
    case opponent_wheel_near: return 0x8808u;
    case opponent_wheel_mid: return 0x8c00u;
    default: return 0;
    }
}

template <class ReadInstruction, std::size_t N>
uint64_t mobile_identity_code_signature(ReadInstruction read, const std::array<uint32_t, N> &sites) {
    uint64_t signature = 14695981039346656037ull;
    for (const auto site : sites) {
        for (uint32_t address = site - 8; address < site + 8; address += 4) {
            const auto instruction = read(address);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                signature ^= (instruction >> shift) & 0xffu;
                signature *= 1099511628211ull;
            }
        }
    }
    return signature;
}

// The reset store, the two geometry writers and their call sites must
// describe the same Revision A program used to derive the key rules.
template <class ReadInstruction>
bool mobile_identity_revision_matches(ReadInstruction read) {
    constexpr std::array sites{
        0xea9cu, 0x17c10u, 0x17d78u, 0x7d68u, 0x7d7cu, 0x78b4u,
        0x810cu, 0x8198u, 0x8210u, 0x8288u, 0x7d94u, 0x8808u,
        0x8c00u, 0x9000u, 0x93dcu, 0x9160u, 0x8d84u, 0x898cu,
    };
    return mobile_identity_code_signature(read, sites) == 0xe1a76ee4c02b02f2ull;
}

template <class ReadInstruction>
bool opponent_wheel_revision_matches(ReadInstruction read) {
    constexpr std::array sites{opponent_wheel_near, opponent_wheel_mid};
    return mobile_identity_code_signature(read, sites) == 0xccb45914e72ba70cull;
}

struct MobileKey {
    uint32_t part = 0, slot = 0, generation = 0;
    bool operator==(const MobileKey &) const = default;
};

// One table per Revision A board. Stamps follow buffer writes, so copying a
// header through RAM or rewriting it with identical data cannot keep its key.
class MobileIdentity {
public:
    static constexpr unsigned word_count = 0x8000, slot_count = 19;

    explicit MobileIdentity(bool opponent_wheels = false) : opponent_wheels_(opponent_wheels) {}

    bool reset_on_tick(uint64_t tick) const { return last_reset_tick_ == tick; }

    void clear_word(unsigned word) {
        if (stamps_[word].key.part) stamps_[word] = {};
    }
    void reset_slot(uint32_t address, uint64_t tick) {
        if (const auto slot = slot_index(address)) {
            if (reset_ticks_[*slot] == tick) return;
            ++generations_[*slot];
            reset_ticks_[*slot] = tick;
            last_reset_tick_ = tick;
        }
    }
    void write_header(unsigned word, uint32_t opcode, uint32_t ip, uint32_t ret,
                      uint32_t address, uint64_t tick) {
        clear_word(word);
        const auto kind = (opcode >> 23) & 31;
        if (kind != 1 && kind != 17) return;

        MobileKey key;
        if (ip == 0x17c10) {
            switch (ret) {
            case 0x7d68: case 0x7d7c: key.part = 0x7d68; break;
            case 0x78b4: case 0x810c: case 0x8198: case 0x8210: case 0x8288:
                key.part = ret; break;
            default: break;
            }
        } else if (ip == 0x17d78 && ret == 0x7d94) {
            key.part = ret;
        }
        if (!key.part) {
            const bool opponent_wheel = opponent_wheels_ && ip == 0x17d78 &&
                                        opponent_wheel_chassis_part(ret) != 0;
            const bool slot_part = opponent_wheel || (ip == 0x17c10 &&
                (ret == 0x8808 || ret == 0x8c00 || ret == 0x9000 || ret == 0x93dc)) ||
                (ip == 0x17d78 && (ret == 0x9160 || ret == 0x8d84 || ret == 0x898c));
            if (!slot_part) return;
            const auto slot = slot_index(address);
            if (!slot || reset_ticks_[*slot] == tick) return;
            key = {ret, address, generations_[*slot]};
        }
        stamps_[word] = {opcode, tick, key};
    }

    MobileKey read(unsigned word, uint32_t opcode, uint64_t tick) const {
        const auto &stamp = stamps_[word];
        if (!stamp.key.part || stamp.opcode != opcode || stamp.tick > tick) return {};
        if (const auto slot = slot_index(stamp.key.slot)) {
            if (generations_[*slot] != stamp.key.generation || reset_ticks_[*slot] == tick) return {};
        }
        return stamp.key;
    }

    static std::optional<unsigned> slot_index(uint32_t address) {
        constexpr uint32_t first = 0x505480, stride = 0x300;
        if (address < first || address >= first + slot_count * stride || (address - first) % stride) return {};
        return (address - first) / stride;
    }
private:
    struct Stamp {
        uint32_t opcode = 0;
        uint64_t tick = 0;
        MobileKey key;
    };
    std::optional<uint64_t> last_reset_tick_;
    std::array<uint32_t, slot_count> generations_{};
    std::array<std::optional<uint64_t>, slot_count> reset_ticks_{};
    std::vector<Stamp> stamps_{word_count};
    bool opponent_wheels_;
};

} // namespace rt
