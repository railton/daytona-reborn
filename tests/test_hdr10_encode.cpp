// PQ ST.2084 and the BT.709 to BT.2020 container matrix. No window and no ROM.

#include "app/hdr10_encode.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "hdr10 encode: %s\n", message);
        ++failures;
    }
}
int code10(float signal) { return int(std::lround(double(signal) * 1023.0)); }
}

int main() {
    const float black = app::hdr10_pq(0.f);
    const float gray = app::hdr10_pq(0.18f);
    const float white = app::hdr10_pq(1.f);
    check(std::fabs(black - 0.f) <= 1e-4f, "black");
    check(std::fabs(gray - 0.409524f) <= 1e-4f, "18 percent gray");
    check(std::fabs(white - 0.579133f) <= 1e-4f, "paper white");
    check(code10(black) == 0 && code10(gray) == 419 && code10(white) == 592, "10-bit codes");
    check(app::kHdr10PaperWhiteNits == 200.f, "paper white is 200 cd/m2");
    check(std::fabs(0.18f * app::kHdr10PaperWhiteNits - 36.f) <= 1e-4f, "18 percent gray is 36 cd/m2");

    float row_sum[3] = {};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) row_sum[row] += app::kBt709ToBt2020[row][col];
    check(std::fabs(row_sum[0] - 1.f) <= 1e-4f && std::fabs(row_sum[1] - 1.f) <= 1e-4f &&
          std::fabs(row_sum[2] - 1.f) <= 1e-4f, "matrix rows sum to 1");
    const app::Hdr10Rgb neutral = app::bt709_to_bt2020({0.18f, 0.18f, 0.18f});
    check(std::fabs(neutral.r - neutral.g) <= 1e-4f && std::fabs(neutral.g - neutral.b) <= 1e-4f &&
          std::fabs(neutral.r - 0.18f) <= 1e-4f, "neutral gray stays neutral");
    const app::Hdr10Rgb red = app::bt709_to_bt2020({1.f, 0.f, 0.f});
    check(std::fabs(red.r - 0.6274f) <= 1e-4f && std::fabs(red.g - 0.0691f) <= 1e-4f &&
          std::fabs(red.b - 0.0164f) <= 1e-4f, "BT.709 red in BT.2020");

    const float gray0 = app::srgb8_to_linear(0);
    const float gray128 = app::srgb8_to_linear(128);
    const float gray255 = app::srgb8_to_linear(255);
    const app::Hdr10Rgb pq0 = app::hdr10_pq_from_srgb8(0, 0, 0);
    const app::Hdr10Rgb pq128 = app::hdr10_pq_from_srgb8(128, 128, 128);
    const app::Hdr10Rgb pq255 = app::hdr10_pq_from_srgb8(255, 255, 255);
    const app::Hdr10Rgb pq_red = app::hdr10_pq_from_srgb8(255, 0, 0);
    check(std::fabs(gray0) <= 1e-4f && code10(pq0.r) == 0 && code10(pq0.g) == 0 && code10(pq0.b) == 0,
          "sRGB gray 0");
    check(std::fabs(gray128 - 0.215861f) <= 1e-4f, "sRGB 128 linear");
    check(std::fabs(pq128.r - 0.426416f) <= 1e-4f && std::fabs(pq128.g - pq128.r) <= 1e-4f &&
          std::fabs(pq128.b - pq128.r) <= 1e-4f, "sRGB 128 PQ");
    check(code10(pq128.r) == 436 && code10(pq128.g) == 436 && code10(pq128.b) == 436, "sRGB 128 code");
    check(code10(pq255.r) == 592 && code10(pq255.g) == 592 && code10(pq255.b) == 592, "sRGB 255 code");
    check(std::fabs(gray255 - 1.f) <= 1e-4f, "sRGB 255 is linear 1");
    check(code10(pq_red.r) == 543 && code10(pq_red.g) == 333 && code10(pq_red.b) == 224, "sRGB red codes");

    const float srgb10 = app::srgb8_to_linear(10);
    const float srgb11 = app::srgb8_to_linear(11);
    check(std::fabs(srgb10 - 0.003035f) <= 1e-6f, "sRGB 10 linear segment");
    check(std::fabs(srgb11 - 0.003347f) <= 1e-6f, "sRGB 11 power segment");

    if (failures) return 1;
    std::printf("hdr10 encode: black %.8g gray18 %.8g (36 cd/m2) white %.8g codes %d %d %d\n",
                black, gray, white, code10(black), code10(gray), code10(white));
    std::printf("hdr10 encode: red709 %.4f %.4f %.4f\n", red.r, red.g, red.b);
    std::printf("hdr10 encode: srgb8 gray0 code %d gray128 linear %.6f pq %.6f code %d gray255 code %d\n",
                code10(pq0.r), gray128, pq128.r, code10(pq128.r), code10(pq255.r));
    std::printf("hdr10 encode: srgb8 red codes %d %d %d\n",
                code10(pq_red.r), code10(pq_red.g), code10(pq_red.b));
    std::printf("hdr10 encode: srgb8 code10 linear %.6f code11 linear %.6f\n", srgb10, srgb11);
    std::puts("hdr10 encode: PQ, neutral gray, BT.709 red and sRGB 8-bit passed");
    return 0;
}
