#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace rt::msc2 {

struct SnapshotFingerprint {
    uint64_t size = 0;
    uint64_t hash = 14695981039346656037ull;

    void append(std::span<const uint8_t> bytes) {
        size += bytes.size();
        for (const auto byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
};

inline constexpr size_t companion_header_size = 32;

struct CompanionHeader {
    uint64_t frame;
    SnapshotFingerprint snapshot;
};

inline std::array<uint8_t, companion_header_size>
encode_companion_header(uint32_t magic, uint64_t frame, SnapshotFingerprint snapshot) {
    std::array<uint8_t, companion_header_size> bytes{};
    const auto put = [&](size_t offset, uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
    };
    put(0, magic, 4);
    put(4, 1, 4);
    put(8, frame, 8);
    put(16, snapshot.size, 8);
    put(24, snapshot.hash, 8);
    return bytes;
}

inline CompanionHeader read_companion_header(std::span<const uint8_t> bytes, uint32_t magic) {
    if (bytes.size() < companion_header_size)
        throw std::runtime_error("MSC2 companion header is truncated");
    const auto get = [&](size_t offset, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
        return value;
    };
    if (get(0, 4) != magic) throw std::runtime_error("invalid MSC2 companion magic");
    if (get(4, 4) != 1) throw std::runtime_error("unsupported version of MSC2 companion");
    const auto size = get(16, 8);
    if (size < 16) throw std::runtime_error("MSC2 companion snapshot is too small");
    return {get(8, 8), {size, get(24, 8)}};
}

inline void verify_companion_snapshot(SnapshotFingerprint expected, std::span<const uint8_t> bytes) {
    if (bytes.size() != expected.size) throw std::runtime_error("MSC2 companion snapshot size mismatch");
    SnapshotFingerprint actual;
    actual.append(bytes);
    if (actual.hash != expected.hash) throw std::runtime_error("MSC2 companion snapshot hash mismatch");
}

} // namespace rt::msc2
