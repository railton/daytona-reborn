#ifndef RT_MSC2_MATERIAL_MEMORY_H
#define RT_MSC2_MATERIAL_MEMORY_H

#include "runtime/msc2_companion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace rt::msc2 {

inline constexpr std::size_t material_header_size = companion_header_size;
inline constexpr std::size_t material_palette_size = 0x800;
inline constexpr std::size_t material_xlat_size = 0xc000;
inline constexpr std::size_t material_luma_size = 0x20000;
inline constexpr std::size_t material_sheet_size = 0x200000;
inline constexpr std::size_t material_file_size =
    material_header_size + material_palette_size + material_xlat_size +
    material_luma_size + material_sheet_size + material_sheet_size;

static_assert(material_file_size == 4376608,
              "msc2 material sidecar must be 4376608 bytes");

inline constexpr uint32_t material_magic = 0x544d324du; // M2MT

inline std::array<std::uint8_t, material_header_size>
encode_material_header(std::uint64_t frame, SnapshotFingerprint snapshot) {
    return encode_companion_header(material_magic, frame, snapshot);
}

struct MaterialView {
    std::uint64_t frame = 0;
    SnapshotFingerprint snapshot{};
    std::span<const std::uint8_t> palette{};
    std::span<const std::uint8_t> xlat{};
    std::span<const std::uint8_t> luma{};
    std::span<const std::uint8_t> tex0{};
    std::span<const std::uint8_t> tex1{};
};

inline MaterialView read_material(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != material_file_size) {
        throw std::runtime_error("msc2 material sidecar has an invalid size");
    }

    const auto header = read_companion_header(bytes, material_magic);
    constexpr std::size_t palette_offset = material_header_size;
    constexpr std::size_t xlat_offset = palette_offset + material_palette_size;
    constexpr std::size_t luma_offset = xlat_offset + material_xlat_size;
    constexpr std::size_t tex0_offset = luma_offset + material_luma_size;
    constexpr std::size_t tex1_offset = tex0_offset + material_sheet_size;

    MaterialView view;
    view.frame = header.frame;
    view.snapshot = header.snapshot;
    view.palette = bytes.subspan(palette_offset, material_palette_size);
    view.xlat = bytes.subspan(xlat_offset, material_xlat_size);
    view.luma = bytes.subspan(luma_offset, material_luma_size);
    view.tex0 = bytes.subspan(tex0_offset, material_sheet_size);
    view.tex1 = bytes.subspan(tex1_offset, material_sheet_size);
    return view;
}

inline void verify_material_snapshot(const MaterialView& view,
                                     std::span<const std::uint8_t> snapshot) {
    verify_companion_snapshot(view.snapshot, snapshot);
}

}  // namespace rt::msc2

#endif  // RT_MSC2_MATERIAL_MEMORY_H
