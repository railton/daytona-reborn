// HDR10 placement of linear SDR. No SDL and no display dependency.
// A value of 1 is kHdr10PaperWhiteNits. BT.709 primaries are written into a
// BT.2020 container with the same colors; the matrix does not widen the gamut.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace app {

inline constexpr float kHdr10PaperWhiteNits = 200.f;
inline constexpr float kHdr10PeakNits = 10000.f;

struct Hdr10Rgb {
    float r = 0.f, g = 0.f, b = 0.f;
};

// Linear BT.709 to linear BT.2020. Each row sums to 1.
inline constexpr float kBt709ToBt2020[3][3] = {
    {0.6274f, 0.3293f, 0.0433f},
    {0.0691f, 0.9195f, 0.0114f},
    {0.0164f, 0.0880f, 0.8956f},
};

inline Hdr10Rgb bt709_to_bt2020(Hdr10Rgb color) {
    Hdr10Rgb out;
    out.r = kBt709ToBt2020[0][0] * color.r + kBt709ToBt2020[0][1] * color.g + kBt709ToBt2020[0][2] * color.b;
    out.g = kBt709ToBt2020[1][0] * color.r + kBt709ToBt2020[1][1] * color.g + kBt709ToBt2020[1][2] * color.b;
    out.b = kBt709ToBt2020[2][0] * color.r + kBt709ToBt2020[2][1] * color.g + kBt709ToBt2020[2][2] * color.b;
    return out;
}

// IEC 61966-2-1. encoded is an sRGB channel in 0..1, not a byte.
inline float srgb_to_linear(float encoded) {
    if (encoded <= 0.04045f) return encoded / 12.92f;
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

inline float srgb8_to_linear(std::uint8_t channel) {
    return srgb_to_linear(float(channel) / 255.f);
}

// SMPTE ST 2084. linear_sdr is 0..1 and is scaled by the paper white.
// The result is the PQ signal, also 0..1.
inline float hdr10_pq(float linear_sdr) {
    constexpr float m1 = 2610.f / 16384.f;
    constexpr float m2 = 2523.f / 32.f;
    constexpr float c1 = 3424.f / 4096.f;
    constexpr float c2 = 2413.f / 128.f;
    constexpr float c3 = 2392.f / 128.f;
    const float nits = std::clamp(linear_sdr, 0.f, 1.f) * kHdr10PaperWhiteNits;
    const float shaped = std::pow(nits / kHdr10PeakNits, m1);
    return std::pow((c1 + c2 * shaped) / (1.f + c3 * shaped), m2);
}

// 8-bit sRGB pixel to three PQ signals: decode, BT.709 to BT.2020, then PQ.
inline Hdr10Rgb hdr10_pq_from_srgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const Hdr10Rgb container = bt709_to_bt2020(
        {srgb8_to_linear(r), srgb8_to_linear(g), srgb8_to_linear(b)});
    return {hdr10_pq(container.r), hdr10_pq(container.g), hdr10_pq(container.b)};
}

} // namespace app
