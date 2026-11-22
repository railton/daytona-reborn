#pragma once

#include "runtime/msc2_companion.h"

namespace rt::msc2 {

inline constexpr int layer_width = 496, layer_height = 384;
inline constexpr size_t layer_image_size = size_t(layer_width) * layer_height * 4;
inline constexpr size_t layer_file_size = companion_header_size + 2 * layer_image_size;
inline constexpr uint32_t layer_magic = 0x594c324du; // M2LY

struct LayerView {
    CompanionHeader header;
    std::span<const uint8_t> background;
    std::span<const uint8_t> foreground;
};

// The returned spans borrow the input buffer.
inline LayerView read_layers(std::span<const uint8_t> bytes) {
    if (bytes.size() != layer_file_size)
        throw std::runtime_error("invalid MSC2 layer companion size");
    return {read_companion_header(bytes, layer_magic),
            bytes.subspan(companion_header_size, layer_image_size),
            bytes.subspan(companion_header_size + layer_image_size, layer_image_size)};
}

} // namespace rt::msc2
