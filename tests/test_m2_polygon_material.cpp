#include "app/gpu/m2_polygon_material.h"

#include <limits>

namespace {

using m2gpu::pack_polygon_material;
using m2gpu::polygon_palette_index;

static_assert(polygon_palette_index(0x3fu) == 0x1000u);
static_assert(polygon_palette_index(0xabcdffc0u) == 0x13ffu);

constexpr auto solid = pack_polygon_material(0, 0, 0, 0, -5, 2, 0xffffu);
static_assert(solid.info == std::array<uint32_t, 4>{4u << 12, 0, 0, 0});
static_assert(solid.more == std::array<uint32_t, 2>{0xfffffffdu, 0x7fffu});

// Mode 3, checker, X mirror suppresses X wrap, Y wrap, microtexture,
// swapped sheets, 256x1024 texture, and the largest sheet offsets.
constexpr auto textured = pack_polygon_material(0xf9ebu, 0xab, 0xb7ffu, 0xe0, -128, 256, 0x9620);
static_assert(textured.info == std::array<uint32_t, 4>{
    3u | 4u | 16u | 32u | 128u | (2u << 8) | (1u << 10) | (7u << 12) | (3u << 17) | (5u << 20),
    2016u | (992u << 16),
    128u | (256u << 16),
    (0xabu << 7) | (0xe0u << 16),
});
static_assert(textured.more == std::array<uint32_t, 2>{128, 0x1620});
static_assert(pack_polygon_material(0, 0, 0, 0, std::numeric_limits<int32_t>::max(), 1, 0).more[0]
              == 0x80000000u);

} // namespace

int main() {}
