#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace m2gpu {

struct PackedPolygonMaterial {
    std::array<uint32_t, 4> info;
    std::array<uint32_t, 2> more;
};

// Palette RAM index: header3 bits 6..15 select the 10-bit palette base;
// the renderer's palette window starts at +0x1000.
inline constexpr uint32_t polygon_palette_index(uint32_t header3) {
    return ((header3 >> 6) & 0x3ffu) + 0x1000u;
}

// Pack the Model 2 polygon texture state exactly as render_one does.
//
// info[0] (see m2.hlsl):
//   bits 0..1   renderer (from texheader[0] bits 13..14)
//   bit  2      checker
//   bit  3      wrap x
//   bit  4      wrap y
//   bit  5      mirror x
//   bit  6      mirror y
//   bit  7      microtexture
//   bits 8..9   microtexture minimum LOD
//   bit  10     texture sheets swapped
//   bits 12..16 maximum mip level
//   bits 17..19 log2(width / 32)
//   bits 20..22 log2(height / 32)
// info[1]: texx | texy << 16
// info[2]: utexx | utexy << 16
// info[3]: luma base in bits 0..15, luma in bits 16..31
// more[0]: texlod (int32 bit pattern)
// more[1]: palette colour (RGB555, bit 15 clear)
inline constexpr PackedPolygonMaterial pack_polygon_material(uint32_t th0, uint32_t th1, uint32_t th2,
                                                             uint8_t luma, int32_t texlod, int32_t lod_bias,
                                                             uint16_t palette_colour) {
    const uint32_t mirx = (th0 >> 8) & 1u, miry = (th0 >> 9) & 1u;
    const uint32_t wlog = th0 & 7u, hlog = (th0 >> 3) & 7u;
    const uint32_t max_level = 4u + std::min(wlog, hlog);
    const uint32_t renderer = (th0 >> 13) & 3u;

    std::array<uint32_t, 4> info{};
    info[0] = renderer | (((th0 >> 15) & 1u) << 2) | ((((th0 >> 6) & 1u) & ~mirx) << 3) |
              ((((th0 >> 7) & 1u) & ~miry) << 4) | (mirx << 5) | (miry << 6) |
              (((th0 >> 12) & 1u) << 7) | (((th0 >> 10) & 3u) << 8) |
              (((th2 >> 12) & 1u) << 10) | (max_level << 12) | (wlog << 17) | (hlog << 20);
    info[1] = (32u * (th2 & 0x3fu)) | ((32u * ((th2 >> 6) & 0x1fu)) << 16);
    info[2] = (((th2 >> 13) & 1u) * 128u) | ((((th2 >> 14) & 3u) * 128u) << 16);
    info[3] = ((th1 & 0xffu) << 7) | (static_cast<uint32_t>(luma) << 16);

    std::array<uint32_t, 2> more{};
    more[0] = static_cast<uint32_t>(texlod) + static_cast<uint32_t>(lod_bias);
    more[1] = static_cast<uint32_t>(palette_colour) & 0x7fffu;

    return PackedPolygonMaterial{info, more};
}

} // namespace m2gpu
