#pragma once

#include "runtime/msc2_companion.h"

namespace rt::msc2 {

inline constexpr uint32_t priority_magic = 0x524f324du; // M2OR
inline constexpr size_t priority_record_size = 12;

struct DrawPriority {
    uint32_t window;
    uint32_t depth_bucket;
    uint32_t emission;
};

inline std::array<uint8_t, priority_record_size> encode_priority(DrawPriority key) {
    std::array<uint8_t, priority_record_size> bytes{};
    const uint32_t values[] = {key.window, key.depth_bucket, key.emission};
    for (size_t i = 0; i < 3; ++i) {
        for (size_t b = 0; b < 4; ++b) bytes[i * 4 + b] = uint8_t(values[i] >> (8 * b));
    }
    return bytes;
}

// The caller validates the companion's exact size and snapshot association.
inline DrawPriority read_priority(std::span<const uint8_t> bytes, size_t index) {
    const size_t start = companion_header_size + index * priority_record_size;
    const auto word = [&](size_t offset) {
        uint32_t value = 0;
        for (size_t b = 0; b < 4; ++b) value |= uint32_t(bytes[start + offset + b]) << (8 * b);
        return value;
    };
    return {word(0), word(4), word(8)};
}

} // namespace rt::msc2
