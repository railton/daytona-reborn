#include "app/gpu/home_scene.h"

#include "app/gpu/home_shaders_gen.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <vector>

namespace app {
namespace {

constexpr float tau = 2 * std::numbers::pi_v<float>;
constexpr float degree = tau / 360;
constexpr auto color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr auto depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

struct Vec {
    float x, y, z;
    Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec operator*(float s) const { return {x * s, y * s, z * s}; }
};

float dot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec cross(Vec a, Vec b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
Vec unit(Vec v) { return v * (1 / std::sqrt(dot(v, v))); }
Vec mix(Vec a, Vec b, float t) { return a + (b - a) * t; }
float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
struct Vec4 { float x, y, z, w; };
Vec4 pack(Vec v, float w) { return {v.x, v.y, v.z, w}; }

struct Vertex { Vec pos, normal; Vec4 color; };
using Mesh = std::vector<Vertex>;

Vec hex(unsigned rgb) {
    return {float((rgb >> 16) & 255) / 255, float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255};
}
// The alpha channel is how much of the car's paint replaces the colour.
Vec4 pigment(unsigned rgb, float paint = 0) { return pack(hex(rgb), paint); }

// ps_home (home.hlsl) lights a face with lerp(shade, sunlit, saturate(n . sun)).
// The sky, the haze and the far hills are given as they should appear on
// screen, so this divides that light back out.
constexpr Vec sun_light{-0.51f, 0.82f, -0.26f};
constexpr Vec shade{0.59f, 0.69f, 0.81f}, sunlit{1.12f, 1.06f, 0.94f};
Vec4 shown(Vec want, Vec normal) {
    const float l = std::clamp(dot(unit(normal), sun_light), 0.f, 1.f);
    const Vec m = mix(shade, sunlit, l);
    return {want.x / m.x, want.y / m.y, want.z / m.z, 0};
}
Vec4 glow(Vec want) { return shown(want, sun_light); }

void tri(Mesh &m, Vec a, Vec b, Vec c, Vec4 color, Vec n) {
    m.insert(m.end(), {{a, n, color}, {b, n, color}, {c, n, color}});
}

void face(Mesh &m, Vec a, Vec b, Vec c, Vec d, Vec4 color) {
    const Vec n = unit(cross(b - a, c - a));
    tri(m, a, b, c, color, n);
    tri(m, a, c, d, color, n);
}

// A quad with a colour per corner, lit as if facing `n`.
void blend(Mesh &m, Vec a, Vec b, Vec c, Vec d, Vec4 ca, Vec4 cb, Vec4 cc, Vec4 cd, Vec n) {
    m.insert(m.end(), {{a, n, ca}, {b, n, cb}, {c, n, cc}, {a, n, ca}, {c, n, cc}, {d, n, cd}});
}

// A face that shows `want` whatever way it faces: lamps, lit panels.
void lamp(Mesh &m, Vec a, Vec b, Vec c, Vec d, Vec want) {
    const Vec n = unit(cross(b - a, c - a));
    tri(m, a, b, c, shown(want, n), n);
    tri(m, a, c, d, shown(want, n), n);
}

template<class Face, class Color>
void box(Mesh &m, Vec a, Vec b, Color c, Face f) {
    const Vec p{a.x, b.y, a.z}, q{a.x, b.y, b.z}, r{b.x, b.y, a.z};
    const Vec s{a.x, a.y, b.z}, t{b.x, a.y, b.z}, u{b.x, a.y, a.z};
    f(m, p, q, b, r, c);
    f(m, a, s, q, p, c);
    f(m, t, u, r, b, c);
    f(m, s, t, b, q, c);
    f(m, u, a, p, r, c);
}
void block(Mesh &m, Vec a, Vec b, Vec4 c) { box(m, a, b, c, face); }
void block(Mesh &m, Vec a, Vec b, Vec want) { box(m, a, b, want, lamp); }

// The oval: a long tri-oval, run with the angle increasing.
Vec track(float a) {
    return {32 * std::cos(a) + 2.5f * std::cos(2*a), 0, 19 * std::sin(a) - 2.8f * std::sin(2*a)};
}

Vec tangent(float a) { return unit(track(a + 0.001f) - track(a - 0.001f)); }
// Positive offsets are toward the infield.
Vec lane(float a, float offset, float height) {
    const Vec t = tangent(a);
    return track(a) + Vec{-t.z * offset, height, t.x * offset};
}

// Distance along the racing line, so the cars keep their gaps through the turns.
struct Path {
    static constexpr int samples = 1024;
    std::array<float, samples + 1> length{};
    Path() {
        for (int i = 1; i <= samples; ++i) {
            const Vec d = track(i * tau / samples) - track((i - 1) * tau / samples);
            length[i] = length[i - 1] + std::sqrt(dot(d, d));
        }
    }
    float total() const { return length[samples]; }
    float angle(float s) const {
        s = std::fmod(s, total());
        if (s < 0) s += total();
        const auto it = std::upper_bound(length.begin(), length.end(), s);
        const int i = std::clamp(int(it - length.begin()), 1, samples);
        const float f = (s - length[i - 1]) / std::max(1e-4f, length[i] - length[i - 1]);
        return (i - 1 + f) * tau / samples;
    }
    float at(float a) const {
        const float f = a / tau * samples;
        const int i = std::clamp(int(f), 0, samples - 1);
        return length[i] + (length[i + 1] - length[i]) * (f - i);
    }
};

const Path &path() {
    static const Path p;
    return p;
}

void strip(Mesh &m, float a, float b, float lo, float hi, float y, unsigned color) {
    face(m, lane(a, hi, y), lane(b, hi, y), lane(b, lo, y), lane(a, lo, y), pigment(color));
}

// A vertical band along the track, facing the infield side.
void wall(Mesh &m, float a, float b, float offset, float lo, float hi, unsigned color) {
    face(m, lane(a, offset, lo), lane(b, offset, lo), lane(b, offset, hi), lane(a, offset, hi), pigment(color));
}

unsigned hash(unsigned x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
float random01(unsigned seed) { return float(hash(seed) & 0xffff) / 65535.f; }

// ---- sky ------------------------------------------------------------------

constexpr float sky_radius = 238;
// Where the sun is drawn: low over the hills, on the side ps_home lights from.
const Vec sun_disc = unit({-0.89f * std::cos(8.5f * degree), std::sin(8.5f * degree), -0.45f * std::cos(8.5f * degree)});
const Vec horizon_haze{0.93f, 0.80f, 0.66f};
const Vec ground_haze{0.67f, 0.68f, 0.58f};

Vec sky_color(Vec d) {
    const float up = std::max(d.y, 0.f);
    const Vec low{0.62f, 0.78f, 0.93f}, high{0.27f, 0.52f, 0.88f}, zenith{0.09f, 0.24f, 0.62f};
    Vec c = up < 0.1f ? mix(horizon_haze, low, smooth(0, 0.1f, up))
          : up < 0.5f ? mix(low, high, smooth(0.1f, 0.5f, up))
          : mix(high, zenith, smooth(0.5f, 1, up));
    const float toward = std::max(0.f, dot(d, sun_disc));
    c = c + Vec{0.42f, 0.20f, -0.02f} * (std::pow(toward, 4.f) * (1 - smooth(0, 0.6f, up)));
    c = c + Vec{0.45f, 0.36f, 0.20f} * std::pow(toward, 48.f);
    if (d.y < 0) c = mix(horizon_haze, ground_haze, smooth(0, 0.12f, -d.y));
    return c;
}

Vec direction(float azimuth, float elevation) {
    return {std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)};
}

void build_sky(Mesh &m) {
    const float rings[] = {-14, -2, 0, 1.5f, 3.5f, 6, 9, 13, 18, 25, 34, 46, 60, 75, 89.9f};
    constexpr int around = 72;
    for (size_t r = 0; r + 1 < std::size(rings); ++r)
        for (int i = 0; i < around; ++i) {
            const float a0 = i * tau / around, a1 = (i + 1) * tau / around;
            const float e0 = rings[r] * degree, e1 = rings[r + 1] * degree;
            const Vec p[] = {direction(a0, e0), direction(a1, e0), direction(a1, e1), direction(a0, e1)};
            blend(m, p[0] * sky_radius, p[1] * sky_radius, p[2] * sky_radius, p[3] * sky_radius,
                  glow(sky_color(p[0])), glow(sky_color(p[1])), glow(sky_color(p[2])), glow(sky_color(p[3])), sun_light);
        }
    // The sun: a white-hot disc in a wide warm halo that fades into the sky.
    const Vec u = unit(cross(sun_disc, {0, 1, 0})), v = cross(u, sun_disc);
    auto around_sun = [&](float radius, float angle) {
        return unit(sun_disc + (u * std::cos(angle) + v * std::sin(angle)) * std::tan(radius * degree));
    };
    constexpr int sides = 40;
    for (int i = 0; i < sides; ++i) {
        const float a0 = i * tau / sides, a1 = (i + 1) * tau / sides;
        const Vec center = sun_disc * (sky_radius - 4);
        tri(m, center, around_sun(2.3f, a0) * (sky_radius - 4), around_sun(2.3f, a1) * (sky_radius - 4),
            glow({1.9f, 1.7f, 1.35f}), sun_light);
        const float halo[] = {2.3f, 3.2f, 6.5f, 14};
        const Vec tint[] = {{1.55f, 1.25f, 0.85f}, {1.25f, 0.98f, 0.66f}, {0, 0, 0}, {0, 0, 0}};
        for (int k = 0; k + 1 < 4; ++k) {
            const Vec p0 = around_sun(halo[k], a0), p1 = around_sun(halo[k], a1);
            const Vec q0 = around_sun(halo[k + 1], a0), q1 = around_sun(halo[k + 1], a1);
            auto shade_at = [&](int ring, Vec d) {
                if (ring < 2) return glow(tint[ring]);
                return glow(ring == 3 ? sky_color(d) : mix(sky_color(d), {1.15f, 0.92f, 0.62f}, 0.6f));
            };
            const float r0 = sky_radius - 3 - k * 0.2f, r1 = sky_radius - 3.2f - k * 0.2f;
            blend(m, p0 * r0, p1 * r0, q1 * r1, q0 * r1, shade_at(k, p0), shade_at(k, p1),
                  shade_at(k + 1, q1), shade_at(k + 1, q0), sun_light);
        }
    }
    // Long thin clouds, their rims the sky's own colour so they have no edge.
    struct Cloud { float azimuth, elevation, width, height; };
    const Cloud clouds[] = {
        {-175, 5.2f, 16, 1.3f}, {-160, 11, 11, 1.0f}, {-141, 4.0f, 20, 1.1f}, {-128, 8.5f, 14, 1.4f},
        {-112, 15, 18, 1.8f}, {-96, 6.5f, 22, 1.2f}, {-82, 21, 15, 2.0f}, {-196, 14, 13, 1.5f},
        {-215, 7.5f, 17, 1.2f}, {-233, 18, 16, 2.0f}, {-67, 10, 12, 1.0f}, {-150, 26, 20, 2.2f},
        {-250, 4.6f, 14, 1.0f}, {-185, 30, 16, 2.4f}, {-120, 36, 22, 2.6f},
    };
    for (const Cloud &c : clouds) {
        const Vec middle = direction(c.azimuth * degree, c.elevation * degree);
        const float warm = std::pow(std::max(0.f, dot(middle, sun_disc)), 3.f);
        const Vec body = mix({0.97f, 0.95f, 0.98f}, {1.18f, 0.90f, 0.62f}, warm);
        const Vec belly = mix({0.78f, 0.80f, 0.90f}, {0.98f, 0.70f, 0.56f}, warm);
        constexpr int n = 18;
        const float radius = sky_radius - 6;
        auto point = [&](float scale, int i) {
            const float a = i * tau / n;
            const float lift = std::sin(a) > 0 ? 1.f : 0.45f;
            return direction((c.azimuth + c.width * scale * std::cos(a)) * degree,
                             (c.elevation + c.height * scale * lift * std::sin(a)) * degree);
        };
        auto color = [&](Vec d, float amount) {
            const Vec cloud = mix(belly, body, smooth(c.elevation - c.height * 0.5f, c.elevation + c.height,
                                                     std::asin(d.y) / degree));
            return glow(mix(sky_color(d), cloud, amount));
        };
        for (int i = 0; i < n; ++i) {
            const Vec in0 = point(0.55f, i), in1 = point(0.55f, i + 1);
            const Vec out0 = point(1, i), out1 = point(1, i + 1);
            m.insert(m.end(), {{middle * radius, sun_light, color(middle, 0.92f)},
                               {in0 * radius, sun_light, color(in0, 0.8f)},
                               {in1 * radius, sun_light, color(in1, 0.8f)}});
            blend(m, in0 * radius, out0 * radius, out1 * radius, in1 * radius,
                  color(in0, 0.8f), color(out0, 0), color(out1, 0), color(in1, 0.8f), sun_light);
        }
    }
}

// ---- world ----------------------------------------------------------------

Vec grass_at(float x, float z) {
    const float r = std::sqrt(x * x / 1.6f + z * z);
    return mix({0.38f, 0.63f, 0.29f}, ground_haze, smooth(55, 170, r));
}

void ground(Mesh &m) {
    const float rings[] = {0, 30, 45, 60, 80, 105, 135, 170, 210, 250};
    constexpr int around = 64;
    for (size_t r = 0; r + 1 < std::size(rings); ++r)
        for (int i = 0; i < around; ++i) {
            const float a0 = i * tau / around, a1 = (i + 1) * tau / around;
            const float r0 = rings[r], r1 = rings[r + 1];
            const Vec p[] = {{r0 * std::cos(a0), -0.06f, r0 * std::sin(a0)}, {r1 * std::cos(a0), -0.06f, r1 * std::sin(a0)},
                             {r1 * std::cos(a1), -0.06f, r1 * std::sin(a1)}, {r0 * std::cos(a1), -0.06f, r0 * std::sin(a1)}};
            auto c = [&](Vec q) { return shown(grass_at(q.x, q.z), {0, 1, 0}); };
            blend(m, p[0], p[3], p[2], p[1], c(p[0]), c(p[3]), c(p[2]), c(p[1]), {0, 1, 0});
        }
    // Mown stripes, fading out with the grass.
    for (int i = -12; i < 12; ++i) {
        const float x0 = i * 6.f, x1 = x0 + 3;
        for (int k = 0; k < 8; ++k) {
            const float z0 = -48 + k * 12.f, z1 = z0 + 12;
            auto c = [&](float x, float z) {
                return shown(mix(grass_at(x, z), {0.47f, 0.71f, 0.34f}, 1 - smooth(40, 75, std::hypot(x / 1.3f, z))),
                             {0, 1, 0});
            };
            blend(m, {x0, -0.045f, z0}, {x0, -0.045f, z1}, {x1, -0.045f, z1}, {x1, -0.045f, z0},
                  c(x0, z0), c(x0, z1), c(x1, z1), c(x1, z0), {0, 1, 0});
        }
    }
}

// Far ranges, flat-coloured and hazy, ringing the horizon.
void ridge(Mesh &m, float radius, float base, float amp, float seed, Vec top, Vec foot) {
    constexpr int segments = 120;
    auto height = [&](float a) {
        return base + amp * (0.55f + 0.25f * std::sin(3 * a + seed) + 0.17f * std::sin(7 * a + 2.3f * seed)
                             + 0.1f * std::sin(17 * a + 4.1f * seed) + 0.06f * std::sin(31 * a + seed));
    };
    for (int i = 0; i < segments; ++i) {
        const float a0 = i * tau / segments, a1 = (i + 1) * tau / segments;
        const float h0 = height(a0), h1 = height(a1);
        const Vec p0{radius * std::cos(a0), h0, radius * std::sin(a0)}, p1{radius * std::cos(a1), h1, radius * std::sin(a1)};
        const float out = radius + 10;
        const Vec f0{out * std::cos(a0), -0.5f, out * std::sin(a0)}, f1{out * std::cos(a1), -0.5f, out * std::sin(a1)};
        const float facet = 0.94f + 0.1f * (0.5f + 0.5f * std::sin(a0 * 23 + seed));
        const Vec t0 = top * facet;
        blend(m, f0, p0, p1, f1, glow(foot), glow(mix(foot, t0, std::min(1.f, h0 / (base + amp)))),
              glow(mix(foot, t0, std::min(1.f, h1 / (base + amp)))), glow(foot), sun_light);
    }
}

void pine(Mesh &m, float x, float z, float h, unsigned tint) {
    block(m, {x - 0.13f, 0, z - 0.13f}, {x + 0.13f, h * 0.6f, z + 0.13f}, pigment(0x82594a));
    for (int layer = 0; layer < 3; ++layer) {
        const float base = h * (0.26f + layer * 0.21f);
        const float radius = h * (0.28f - layer * 0.054f);
        for (int i = 0; i < 6; ++i) {
            const float a = i * tau / 6, b = (i + 1) * tau / 6;
            const Vec p{x + radius * std::cos(a), base, z + radius * std::sin(a)};
            const Vec q{x + radius * std::cos(b), base, z + radius * std::sin(b)};
            const Vec tip{x, base + h * 0.34f, z};
            tri(m, p, tip, q, pigment(tint), unit(cross(tip - p, q - p)));
        }
    }
}

void palm(Mesh &m, Vec base, float h, float yaw, unsigned seed) {
    const Vec lean{std::cos(yaw), 0, std::sin(yaw)};
    auto spine = [&](float t) { return base + Vec{0, h * t, 0} + lean * (0.18f * h * t * t); };
    constexpr int pieces = 4;
    for (int i = 0; i < pieces; ++i) {
        const float t0 = float(i) / pieces, t1 = float(i + 1) / pieces;
        const Vec p = spine(t0), q = spine(t1);
        const float w0 = 0.2f - 0.07f * t0, w1 = 0.2f - 0.07f * t1;
        const Vec corners[] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
        for (int k = 0; k < 4; ++k) {
            const Vec a = corners[k], b = corners[(k + 1) % 4];
            face(m, p + a * w0, p + b * w0, q + b * w1, q + a * w1, pigment(i % 2 ? 0x9a7656 : 0x8a6748));
        }
    }
    const Vec top = spine(1);
    block(m, top - Vec{0.22f, 0.25f, 0.22f}, top + Vec{0.22f, 0.08f, 0.22f}, pigment(0x5b4630));
    const unsigned greens[] = {0x2f9a52, 0x3fae58, 0x26864d};
    constexpr int fronds = 8;
    for (int i = 0; i < fronds; ++i) {
        const float a = yaw + i * tau / fronds + random01(seed + i) * 0.4f;
        const float len = h * (0.42f + 0.12f * random01(seed * 7 + i));
        const Vec out{std::cos(a), 0, std::sin(a)}, side{-out.z, 0, out.x};
        const Vec mid = top + out * (len * 0.5f) + Vec{0, len * 0.16f, 0};
        const Vec tip = top + out * len + Vec{0, -len * 0.42f, 0};
        const Vec4 c = pigment(greens[(seed + i) % 3]);
        tri(m, top, mid + side * (len * 0.2f), tip, c, unit(cross(mid + side - top, tip - top)));
        tri(m, top, tip, mid - side * (len * 0.2f), c, unit(cross(tip - top, mid - side - top)));
    }
}

void bush(Mesh &m, float x, float z, float r, unsigned tint) {
    for (int i = 0; i < 6; ++i) {
        const float a = i * tau / 6, b = (i + 1) * tau / 6;
        const Vec p{x + r * std::cos(a), 0, z + r * std::sin(a)}, q{x + r * std::cos(b), 0, z + r * std::sin(b)};
        const Vec pm{x + 0.7f * r * std::cos(a), 0.55f * r, z + 0.7f * r * std::sin(a)};
        const Vec qm{x + 0.7f * r * std::cos(b), 0.55f * r, z + 0.7f * r * std::sin(b)};
        face(m, p, q, qm, pm, pigment(tint));
        tri(m, pm, qm, {x, 0.85f * r, z}, pigment(tint), unit(cross(qm - pm, Vec{x, 0.85f * r, z} - pm)));
    }
}

constexpr float start_line = -1.40f;

void build_track(Mesh &m) {
    constexpr int pieces = 200;
    for (int i = 0; i < pieces; ++i) {
        const float a = i * tau / pieces, b = (i+1) * tau / pieces;
        strip(m, a, b, -6.0f, 6.0f, 0.015f, i % 3 ? 0x4d515c : 0x51555f);
        strip(m, a, b, -6.55f, -6.0f, 0.065f, (i / 2) % 2 ? 0xf4f5f0 : 0xe4473e);
        strip(m, a, b, 6.0f, 6.55f, 0.065f, (i / 2) % 2 ? 0xf4f5f0 : 0xe4473e);
        strip(m, a, b, 6.55f, 8.4f, 0.02f, 0x8b9099);       // apron
        strip(m, a, b, 6.7f, 6.86f, 0.03f, 0xf2c534);       // yellow line on the apron
        if (i % 4 < 2) strip(m, a, b, -0.06f, 0.06f, 0.034f, 0xd2d5dc);
        strip(m, a, b, -5.67f, -5.53f, 0.032f, 0xeef0f4);
        strip(m, a, b, 5.53f, 5.67f, 0.032f, 0xeef0f4);
        // Outer wall, white with a red band, and the catch fence above it.
        const float rail = -7.03f;
        wall(m, a, b, rail, 0, 1.15f, 0xeceff3);
        wall(m, a, b, rail + 0.01f, 0.78f, 0.95f, 0xd23c39);
        strip(m, a, b, rail - 0.18f, rail + 0.05f, 1.15f, 0xf7f8fa);
        wall(m, a, b, rail - 0.1f, 3.25f, 3.35f, 0xb4bfca);
        wall(m, a, b, rail - 0.1f, 2.15f, 2.2f, 0x9aa6b2);
        if (i % 3 == 0) {
            const Vec p = lane(a, rail - 0.1f, 0);
            block(m, p + Vec{-0.06f,1.1f,-0.06f}, p + Vec{0.06f,3.4f,0.06f}, pigment(0x8d99a5));
        }
    }
    // Checkered start line, and the gantry over it with its start lights.
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 16; ++i) {
            const float a = start_line + row * 0.022f, s = -5.52f + i * 0.69f;
            strip(m, a, a + 0.022f, s, s + 0.69f, 0.041f, (row+i) % 2 ? 0x1e232e : 0xf6f6f0);
        }
    {
        const float a = start_line + 0.011f;
        for (float side : {-7.6f, 8.9f}) {
            const Vec p = lane(a, side, 0);
            block(m, p + Vec{-0.3f, 0, -0.3f}, p + Vec{0.3f, 6.6f, 0.3f}, pigment(0xdfe5ec));
        }
        constexpr int cells = 18;
        for (int i = 0; i < cells; ++i) {
            const float s0 = -7.6f + i * 16.5f / cells, s1 = s0 + 16.5f / cells;
            for (int row = 0; row < 2; ++row) {
                const Vec4 c = pigment((i + row) % 2 ? 0x161a22 : 0xf5f5f2);
                const float y0 = 5.6f + row * 0.5f;
                face(m, lane(a + 0.012f, s0, y0), lane(a + 0.012f, s1, y0),
                     lane(a + 0.012f, s1, y0 + 0.5f), lane(a + 0.012f, s0, y0 + 0.5f), c);
                face(m, lane(a - 0.012f, s1, y0), lane(a - 0.012f, s0, y0),
                     lane(a - 0.012f, s0, y0 + 0.5f), lane(a - 0.012f, s1, y0 + 0.5f), c);
            }
        }
        strip(m, a - 0.012f, a + 0.012f, -7.6f, 8.9f, 6.6f, 0x2a3242);
        for (int i = 0; i < 5; ++i) {
            const Vec p = lane(a + 0.014f, -2.2f + i * 1.1f, 5.0f);
            block(m, p - Vec{0.38f, 0.38f, 0.1f}, p + Vec{0.38f, 0.38f, 0.1f}, pigment(0x181c24));
            const Vec q = lane(a + 0.02f, -2.2f + i * 1.1f, 5.0f);
            block(m, q - Vec{0.24f, 0.24f, 0.05f}, q + Vec{0.24f, 0.24f, 0.05f}, Vec{1.9f, 0.28f, 0.18f});
        }
    }
}

// Grandstand along the outside of the main straight, crowd and all.
void build_grandstand(Mesh &m) {
    constexpr float from = -2.30f, to = -0.70f;
    constexpr int pieces = 40, tiers = 13;
    const unsigned crowd[] = {0xf3f1ea, 0xe3483f, 0xf6c642, 0x3b6fd6, 0xf08a3c, 0xe86aa6, 0x46b07a, 0x24324a};
    for (int i = 0; i < pieces; ++i) {
        const float a = from + (to - from) * i / pieces, b = from + (to - from) * (i + 1) / pieces;
        for (int k = 0; k < tiers; ++k) {
            const float lo = -9.0f - k * 1.05f, hi = lo - 1.05f;
            const float y0 = 0.6f + k * 0.72f;
            wall(m, a, b, lo, y0 - 0.72f, y0, k % 2 ? 0x3d63b0 : 0x4a74c4);
            face(m, lane(a, lo, y0), lane(b, lo, y0), lane(b, hi, y0), lane(a, hi, y0),
                 pigment(k % 2 ? 0x6f8fc8 : 0x7d9cd2));
            for (int s = 0; s < 3; ++s) {
                const unsigned seed = unsigned(i * 977 + k * 131 + s * 17);
                if (random01(seed) < 0.18f) continue;
                const float a0 = a + (b - a) * (s / 3.f + 0.04f), a1 = a0 + (b - a) * 0.24f;
                const float o0 = lo - 0.2f - 0.3f * random01(seed + 3), o1 = o0 - 0.45f;
                face(m, lane(a0, o0, y0 + 0.45f), lane(a1, o0, y0 + 0.45f), lane(a1, o0, y0 + 0.02f),
                     lane(a0, o0, y0 + 0.02f), pigment(crowd[hash(seed) % std::size(crowd)]));
                strip(m, a0, a1, o1, o0, y0 + 0.45f, crowd[hash(seed + 1) % std::size(crowd)]);
            }
        }
        const float back = -9.0f - tiers * 1.05f;
        wall(m, a, b, back, 0, 12.5f, 0xd9e1ea);
        wall(m, a, b, -9.0f, 0, 0.6f, 0x2b3d63);
        // Roof: white canopy, red fascia, on slim columns.
        face(m, lane(a, -11.5f, 12.2f), lane(b, -11.5f, 12.2f), lane(b, back - 0.6f, 12.9f),
             lane(a, back - 0.6f, 12.9f), pigment(0xf1f3f6));
        wall(m, a, b, -11.5f, 11.6f, 12.25f, i % 2 ? 0xd8423b : 0xdf4f43);
        if (i % 4 == 0) {
            const Vec p = lane(a, back + 0.4f, 0);
            block(m, p - Vec{0.12f, 0, 0.12f}, p + Vec{0.12f, 12.6f, 0.12f}, pigment(0xc3ccd6));
        }
    }
    // Press box over the finish line.
    const float a = start_line - 0.08f, b = start_line + 0.10f;
    for (int k = 0; k < 2; ++k) {
        const float y = 12.9f + k * 2.2f;
        face(m, lane(a, -14.5f, y), lane(b, -14.5f, y), lane(b, -14.5f, y + 2.2f), lane(a, -14.5f, y + 2.2f),
             pigment(0x203554));
        lamp(m, lane(a, -14.49f, y + 0.4f), lane(b, -14.49f, y + 0.4f), lane(b, -14.49f, y + 1.7f),
             lane(a, -14.49f, y + 1.7f), {0.62f, 0.82f, 0.95f});
    }
    face(m, lane(a, -14.3f, 17.3f), lane(b, -14.3f, 17.3f), lane(b, -22.5f, 17.3f), lane(a, -22.5f, 17.3f),
         pigment(0xeef1f5));
    for (float s : {a, b}) wall(m, s - 0.002f, s + 0.002f, -14.5f, 12.9f, 17.3f, 0x203554);
    // Light towers behind the stands.
    for (int i = 0; i < 4; ++i) {
        const float t = from + 0.15f + i * (to - from - 0.3f) / 3;
        const Vec p = lane(t, -25.5f, 0);
        block(m, p - Vec{0.25f, 0, 0.25f}, p + Vec{0.25f, 21, 0.25f}, pigment(0x9aa7b5));
        // A bank of lamps facing the track.
        face(m, lane(t - 0.045f, -25.0f, 19.6f), lane(t + 0.045f, -25.0f, 19.6f), lane(t + 0.045f, -25.0f, 22.4f),
             lane(t - 0.045f, -25.0f, 22.4f), pigment(0x273041));
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) {
                const float a0 = t - 0.038f + col * 0.0195f, a1 = a0 + 0.015f, y = 19.85f + row * 0.85f;
                lamp(m, lane(a0, -24.9f, y), lane(a1, -24.9f, y), lane(a1, -24.9f, y + 0.6f), lane(a0, -24.9f, y + 0.6f),
                     {1.6f, 1.5f, 1.25f});
            }
    }
}

// Pit road and garages inside the main straight, the scoring pylon, the lake.
void build_infield(Mesh &m) {
    constexpr float from = -2.05f, to = -0.85f;
    constexpr int pieces = 30;
    for (int i = 0; i < pieces; ++i) {
        const float a = from + (to - from) * i / pieces, b = from + (to - from) * (i + 1) / pieces;
        strip(m, a, b, 8.4f, 11.0f, 0.02f, 0x5d626d);
        strip(m, a, b, 8.4f, 8.52f, 0.03f, 0xeef0f4);
        wall(m, a, b, 11.6f, 0, 2.6f, i % 2 ? 0xe9e2d4 : 0xe3dccd);
        const float door0 = a + (b - a) * 0.15f, door1 = a + (b - a) * 0.85f;
        face(m, lane(door0, 11.59f, 0.05f), lane(door1, 11.59f, 0.05f), lane(door1, 11.59f, 1.9f),
             lane(door0, 11.59f, 1.9f), pigment(i % 3 ? 0x3a5f8d : 0x2f4f78));
        strip(m, a, b, 11.3f, 15.4f, 2.6f, 0xf3f4f6);
        wall(m, a, b, 11.3f, 2.25f, 2.75f, 0xe34a3f);
    }
    // Tower by the line, glass band near the top.
    const Vec base = lane(start_line, 13.6f, 0);
    block(m, base - Vec{1.6f, 0, 1.6f}, base + Vec{1.6f, 6.2f, 1.6f}, pigment(0xe9ecf0));
    block(m, base - Vec{1.75f, -6.2f, 1.75f}, base + Vec{1.75f, 7.6f, 1.75f}, Vec{0.55f, 0.76f, 0.92f});
    block(m, base - Vec{2.0f, -7.6f, 2.0f}, base + Vec{2.0f, 8.0f, 2.0f}, pigment(0xd8463e));

    // Scoring pylon: a slim tower with rows of lit positions.
    const Vec pylon{-17.5f, 0, -4.5f};
    block(m, pylon - Vec{0.6f, 0, 0.6f}, pylon + Vec{0.6f, 9, 0.6f}, pigment(0xc9d1da));
    block(m, pylon - Vec{1.5f, -9, 1.5f}, pylon + Vec{1.5f, 21, 1.5f}, pigment(0x1b2230));
    block(m, pylon - Vec{1.7f, -21, 1.7f}, pylon + Vec{1.7f, 22.4f, 1.7f}, pigment(0xd8463e));
    for (int row = 0; row < 10; ++row) {
        const float y = 10.0f + row * 1.08f;
        const Vec c = row % 3 == 0 ? Vec{1.5f, 1.05f, 0.35f} : Vec{1.25f, 1.25f, 1.2f};
        for (Vec n : {Vec{1, 0, 0}, Vec{0, 0, 1}, Vec{-1, 0, 0}, Vec{0, 0, -1}}) {
            const Vec s{-n.z, 0, n.x};
            const Vec o = pylon + n * 1.52f + Vec{0, y, 0};
            lamp(m, o - s * 1.1f, o + s * 1.1f, o + s * 1.1f + Vec{0, 0.6f, 0}, o - s * 1.1f + Vec{0, 0.6f, 0}, c);
        }
    }

    // The lake, brighter toward the low sun.
    const Vec centre{-5.5f, -0.03f, 4.2f};
    constexpr float rx = 15.5f, rz = 6.2f;
    constexpr int around = 48;
    for (int i = 0; i < around; ++i) {
        const float a0 = i * tau / around, a1 = (i + 1) * tau / around;
        auto at = [&](float a, float s, float y) {
            const float wobble = 1 + 0.06f * std::sin(3 * a + 1) + 0.04f * std::sin(5 * a);
            return Vec{centre.x + rx * s * wobble * std::cos(a), y, centre.z + rz * s * wobble * std::sin(a)};
        };
        auto water = [&](Vec p, float depth) {
            const float glint = std::pow(std::clamp(-(p.x - centre.x) / rx, 0.f, 1.f), 2.f) * 0.55f;
            return shown(mix(mix({0.32f, 0.64f, 0.77f}, {0.12f, 0.36f, 0.60f}, depth), {1.0f, 0.82f, 0.58f}, glint),
                         {0, 1, 0});
        };
        const Vec c0 = at(a0, 0, -0.03f);
        tri(m, c0, at(a1, 0.6f, -0.03f), at(a0, 0.6f, -0.03f), water(c0, 1), {0, 1, 0});
        m[m.size() - 2].color = water(at(a1, 0.6f, 0), 0.8f);
        m[m.size() - 1].color = water(at(a0, 0.6f, 0), 0.8f);
        blend(m, at(a0, 0.6f, -0.03f), at(a1, 0.6f, -0.03f), at(a1, 1, -0.03f), at(a0, 1, -0.03f),
              water(at(a0, 0.6f, 0), 0.8f), water(at(a1, 0.6f, 0), 0.8f), water(at(a1, 1, 0), 0),
              water(at(a0, 1, 0), 0), {0, 1, 0});
        face(m, at(a0, 1, -0.035f), at(a1, 1, -0.035f), at(a1, 1.12f, -0.035f), at(a0, 1.12f, -0.035f),
             pigment(0xe6d6a8));
    }
}

void build_scenery(Mesh &m) {
    // Two hazy ranges, the far one bluer.
    ridge(m, 205, 9, 26, 0.7f, {0.55f, 0.58f, 0.74f}, mix(horizon_haze, {0.70f, 0.66f, 0.70f}, 0.4f));
    ridge(m, 150, 2, 11, 2.9f, {0.36f, 0.50f, 0.50f}, mix(ground_haze, {0.48f, 0.56f, 0.50f}, 0.5f));
    const unsigned greens[] = {0x2c7f5a, 0x3f9360, 0x55a160, 0x21694f};
    // Pines in a broad belt outside the oval, palms and bushes nearer in.
    for (int i = 0; i < 220; ++i) {
        const float a = float((i * 173) % 211) * tau / 211;
        const float offset = -30.f - float((i * 113) % 97) * 0.45f;
        const Vec p = lane(a, offset, 0);
        if (p.x > 28 && std::abs(p.z) < 50) continue; // keep the camera's foreground clear
        pine(m, p.x, p.z, 3.0f + float(i % 7) * 0.6f, greens[i % 4]);
    }
    for (int i = 0; i < 70; ++i) {
        const float a = float((i * 37) % 71) * tau / 71;
        const float offset = -11.5f - float((i * 29) % 13) * 1.1f;
        if (a > tau + -2.45f || (a > 3.83f && a < 5.6f)) continue; // the grandstand
        const Vec p = lane(a, offset, 0);
        if (p.x > 24) continue; // the camera's foreground
        palm(m, p, 4.5f + random01(i * 11u) * 2.5f, random01(i * 5u) * tau, unsigned(i));
    }
    for (int i = 0; i < 26; ++i) {
        const float a = i * tau / 26 + 0.1f;
        if (std::sin(a) < -0.35f) continue; // pit road
        const Vec p = lane(a, 13.5f + 2.f * random01(i * 3u), 0);
        palm(m, p, 4.0f + random01(i * 13u) * 2.0f, random01(i * 7u) * tau, unsigned(i + 100));
        bush(m, p.x + 1.4f, p.z - 0.8f, 0.9f, greens[i % 4]);
    }
}

void build_world(Mesh &m) {
    ground(m);
    build_track(m);
    build_grandstand(m);
    build_infield(m);
    build_scenery(m);
}

// A stock car, nose toward +z; parts with paint take the car's colour.
void build_car(Mesh &m) {
    block(m, {-1.12f,0.015f,-2.25f}, {1.12f,0.027f,2.25f}, pigment(0x2c2f38));          // shadow
    block(m, {-0.96f,0.18f,-2.18f}, {0.96f,0.40f,2.12f}, pigment(0x000000, 0.72f));      // lower body
    block(m, {-0.97f,0.40f,-2.2f}, {0.97f,0.66f,0.95f}, pigment(0xffffff, 1));           // body
    // Hood sloping to the nose.
    const Vec h0{-0.97f,0.66f,0.95f}, h1{0.97f,0.66f,0.95f}, h2{0.94f,0.52f,2.14f}, h3{-0.94f,0.52f,2.14f};
    face(m, h0, h1, h2, h3, pigment(0xffffff, 1));
    face(m, {-0.97f,0.40f,0.95f}, h0, h3, {-0.96f,0.40f,2.14f}, pigment(0xffffff, 1));
    face(m, h1, {0.97f,0.40f,0.95f}, {0.96f,0.40f,2.14f}, h2, pigment(0xffffff, 1));
    face(m, h3, h2, {0.96f,0.40f,2.14f}, {-0.96f,0.40f,2.14f}, pigment(0xffffff, 1));
    block(m, {-0.98f,0.16f,2.05f}, {0.98f,0.24f,2.24f}, pigment(0x14161c));              // splitter
    // Cabin.
    const Vec a{-0.84f,0.66f,-0.95f}, b{-0.84f,0.66f,0.72f}, c{0.84f,0.66f,0.72f}, d{0.84f,0.66f,-0.95f};
    const Vec A{-0.66f,1.12f,-0.62f}, B{-0.66f,1.12f,0.12f}, C{0.66f,1.12f,0.12f}, D{0.66f,1.12f,-0.62f};
    face(m, A,B,C,D,pigment(0xffffff, 1));
    face(m, b,c,C,B,pigment(0x22374f));
    face(m, d,a,A,D,pigment(0x1d2f45));
    face(m, a,b,B,A,pigment(0x182638));
    face(m, c,d,D,C,pigment(0x182638));
    // Rear spoiler and the deck under it.
    block(m, {-0.95f,0.66f,-2.2f}, {0.95f,0.92f,-2.12f}, pigment(0x14161c));
    // Number roundels on the roof and the doors, a stripe down the middle.
    auto roundel = [&](Vec centre, Vec u, Vec v, float r) {
        const Vec n = unit(cross(u, v));
        for (int i = 0; i < 10; ++i) {
            const float a0 = i * tau / 10, a1 = (i + 1) * tau / 10;
            tri(m, centre, centre + (u * std::cos(a0) + v * std::sin(a0)) * r,
                centre + (u * std::cos(a1) + v * std::sin(a1)) * r, pigment(0xf6f6f2), n);
        }
        block(m, centre - u * (r * 0.12f) - v * (r * 0.55f) + n * 0.002f,
              centre + u * (r * 0.12f) + v * (r * 0.55f) + n * 0.004f, pigment(0x1a1d24));
    };
    roundel({0, 1.123f, -0.25f}, {1, 0, 0}, {0, 0, 1}, 0.4f);
    roundel({0.975f, 0.5f, -0.2f}, {0, 0, 1}, {0, 1, 0}, 0.27f);
    roundel({-0.975f, 0.5f, -0.2f}, {0, 0, -1}, {0, 1, 0}, 0.27f);
    face(m, {-0.18f,0.665f,0.95f}, {0.18f,0.665f,0.95f}, {0.17f,0.525f,2.14f}, {-0.17f,0.525f,2.14f},
         pigment(0xf6f6f2));
    block(m, {-0.18f,0.66f,-2.12f}, {0.18f,0.665f,-0.95f}, pigment(0xf6f6f2));
    for (float x : {-0.66f, 0.66f}) {
        block(m, {x-0.22f,0.42f,2.135f}, {x+0.22f,0.52f,2.16f}, pigment(0xfff4cf));        // headlight decals
        block(m, {x-0.22f,0.48f,-2.205f}, {x+0.22f,0.60f,-2.19f}, pigment(0xd8302c));      // tail lights
    }
    for (float x : {-0.86f, 0.86f}) {
        for (float z : {-1.32f, 1.35f}) {
            constexpr int sides = 10;
            for (int i = 0; i < sides; ++i) {
                const float a0 = i * tau / sides, a1 = (i + 1) * tau / sides;
                const Vec outer{x + std::copysign(0.14f,x),0.33f,z};
                const Vec p = outer + Vec{0, 0.31f * std::cos(a0), 0.31f * std::sin(a0)};
                const Vec q = outer + Vec{0, 0.31f * std::cos(a1), 0.31f * std::sin(a1)};
                const Vec width{-std::copysign(0.30f,x),0,0};
                const Vec n{0,std::cos((a0+a1)/2),std::sin((a0+a1)/2)};
                tri(m,p,q,q+width,pigment(0x1d1f27),n);
                tri(m,p,q+width,p+width,pigment(0x1d1f27),n);
                tri(m,p,q,outer,pigment(0x30333f),{std::copysign(1.f,x),0,0});
                tri(m,(p-outer)*0.55f+outer+Vec{std::copysign(0.003f,x),0,0},
                    (q-outer)*0.55f+outer+Vec{std::copysign(0.003f,x),0,0},
                    outer+Vec{std::copysign(0.003f,x),0,0},pigment(0xc9ced6),{std::copysign(1.f,x),0,0});
            }
        }
    }
}

template<size_t S, size_t D>
SDL_GPUShader *shader(SDL_GPUDevice *dev, SDL_GPUShaderStage stage, Uint32 uniforms, Uint32 samplers,
                      const unsigned char (&spv)[S], const unsigned char (&dxil)[D], const char *entry) {
    SDL_GPUShaderCreateInfo info{};
    info.stage = stage;
    info.num_uniform_buffers = uniforms;
    info.num_samplers = samplers;
    const auto formats = SDL_GetGPUShaderFormats(dev);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spv;
        info.code_size = S;
        info.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = dxil;
        info.code_size = D;
        info.entrypoint = entry;
    } else {
        return nullptr;
    }
    return SDL_CreateGPUShader(dev, &info);
}

SDL_GPUGraphicsPipeline *pipeline(SDL_GPUDevice *dev, SDL_GPUShader *vs, SDL_GPUShader *ps,
                                  SDL_GPUTextureFormat format, bool geometry) {
    if (!vs || !ps) return nullptr;
    SDL_GPUVertexBufferDescription stream{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attrs[] = {
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,pos)},
        {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,normal)},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(Vertex,color)},
    };
    SDL_GPUColorTargetDescription color{};
    color.format = format;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs;
    info.fragment_shader = ps;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    if (geometry) {
        info.vertex_input_state = {&stream, 1, attrs, 3};
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.depth_stencil_state.enable_depth_test = info.depth_stencil_state.enable_depth_write = true;
        info.target_info.depth_stencil_format = depth_format;
        info.target_info.has_depth_stencil_target = true;
    }
    return SDL_CreateGPUGraphicsPipeline(dev, &info);
}

// The field: who runs where, in arc length behind the leader and lane offset.
struct Slot { float behind, lane; unsigned color; };
constexpr Slot field[] = {
    {0, 2.4f, 0xe8392f},     {6.2f, 2.4f, 0x2f6fe0},   {12.4f, 2.3f, 0xf4c02c},  {12.9f, -2.5f, 0xf3efe6},
    {18.8f, 2.4f, 0x1fae7e}, {19.4f, -2.4f, 0xf07a2a}, {25.4f, 2.3f, 0x7a5be0},  {26.0f, -2.5f, 0xe95f9c},
    {33.5f, 0.2f, 0x23b8d8}, {48.0f, 2.2f, 0xd9dde4},
};
constexpr float race_speed = 23.f; // world units per second

} // namespace

bool HomeScene::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat swap_format) {
    dev_ = dev;
    auto *vs = shader(dev, SDL_GPU_SHADERSTAGE_VERTEX, 2, 0, k_vs_home_spv, k_vs_home_dxil, "vs_home");
    auto *ps = shader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, k_ps_home_spv, k_ps_home_dxil, "ps_home");
    auto *lens_vs = shader(dev, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, k_vs_lens_spv, k_vs_lens_dxil, "vs_lens");
    auto *lens_ps = shader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, k_ps_lens_spv, k_ps_lens_dxil, "ps_lens");
    world_ = pipeline(dev, vs, ps, color_format, true);
    lens_ = pipeline(dev, lens_vs, lens_ps, swap_format, false);
    for (auto *s : {vs, ps, lens_vs, lens_ps}) if (s) SDL_ReleaseGPUShader(dev, s);
    if (!world_ || !lens_) return false;

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev, &sampler);
    Mesh mesh;
    build_world(mesh);
    track_vertices_ = Uint32(mesh.size());
    build_sky(mesh);
    sky_vertices_ = Uint32(mesh.size()) - track_vertices_;
    build_car(mesh);
    car_vertices_ = Uint32(mesh.size()) - track_vertices_ - sky_vertices_;
    const Uint32 bytes = Uint32(mesh.size() * sizeof(Vertex));
    SDL_GPUBufferCreateInfo vertex_info{SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0};
    vertices_ = SDL_CreateGPUBuffer(dev, &vertex_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    auto *transfer = SDL_CreateGPUTransferBuffer(dev, &transfer_info);
    bool uploaded = false;
    if (sampler_ && vertices_ && transfer) {
        if (void *p = SDL_MapGPUTransferBuffer(dev, transfer, false)) {
            std::memcpy(p, mesh.data(), bytes);
            SDL_UnmapGPUTransferBuffer(dev, transfer);
            if (auto *cmd = SDL_AcquireGPUCommandBuffer(dev)) {
                auto *copy = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTransferBufferLocation from{transfer,0};
                SDL_GPUBufferRegion to{vertices_,0,bytes};
                SDL_UploadToGPUBuffer(copy, &from, &to, false);
                SDL_EndGPUCopyPass(copy);
                uploaded = SDL_SubmitGPUCommandBuffer(cmd);
            }
        }
    }
    if (transfer) SDL_ReleaseGPUTransferBuffer(dev, transfer);
    return uploaded;
}

bool HomeScene::resize(Uint32 w, Uint32 h) {
    if (w == w_ && h == h_) return color_ && depth_;
    w_ = w;
    h_ = h;
    if (color_) SDL_ReleaseGPUTexture(dev_, color_);
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, color_format,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
    color_ = SDL_CreateGPUTexture(dev_, &info);
    info.format = depth_format;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_ = SDL_CreateGPUTexture(dev_, &info);
    if (!color_ || !depth_) SDL_Log("Home scene targets: %s", SDL_GetError());
    return color_ && depth_;
}

bool HomeScene::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swap, Uint32 w, Uint32 h,
                       double seconds, float visibility) {
    if (!resize(w, h)) return false;
    const float t = float(seconds);
    const Path &line = path();
    // The pack: the leader's distance round the lap, from a start that puts
    // it at the exit of the near turn a few seconds in.
    const float lead = line.at(0.35f) - race_speed * 3.0f + race_speed * float(std::fmod(seconds, 600.0));
    // The camera swings slowly round the near turn, rising and dipping,
    // never quite repeating (periods 47 s, 61 s, 73 s).
    const float swing = std::sin(t * tau / 47), rise = std::sin(t * tau / 61 + 1.1f), drift = std::sin(t * tau / 73 + 2.f);
    const float bearing = (18.f + 10.f * swing) * degree, distance = 67.f + 4.f * drift;
    const Vec eye{distance * std::cos(bearing), 12.f + 3.f * rise, distance * std::sin(bearing)};
    const Vec look{-6.f + 4.f * drift, 1.0f + 0.8f * rise, -2.f + 3.f * swing};
    const Vec forward = unit(look - eye), right = unit(cross(forward, {0,1,0})), up = cross(right,forward);
    const float aspect = float(w) / float(h);
    const float shift = std::clamp((aspect - 0.85f) * 0.33f, 0.0f, 0.41f);
    // Focus follows the pack a little, a wide band around the look point.
    const Vec pack_at = lane(line.angle(lead - 12.f), 0, 0);
    const float focus_depth = dot(forward, mix(look, pack_at, 0.4f));
    constexpr float focus_scale = 0.42f; // shrinks distances: a wider sharp band, a gentler falloff
    const Vec4 camera[] = {pack(eye,0),pack(right,2.04f/aspect),pack(up,2.04f),pack(forward,shift),
                           pack(forward * focus_scale, focus_depth * focus_scale)};
    SDL_GPUColorTargetInfo ct{};
    ct.texture = color_;
    ct.clear_color = {horizon_haze.x, horizon_haze.y, horizon_haze.z, 1};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    auto *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_BindGPUGraphicsPipeline(pass, world_);
    SDL_GPUBufferBinding binding{vertices_,0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, camera, sizeof camera);
    const Vec4 track_pose[] = {{0,0,0,1},{0,0,0,0}};
    SDL_PushGPUVertexUniformData(cmd, 1, track_pose, sizeof track_pose);
    SDL_DrawGPUPrimitives(pass, track_vertices_, 1, 0, 0);
    const Vec4 sky_pose[] = {{eye.x, 0, eye.z, 1}, {0,0,0,0}};
    SDL_PushGPUVertexUniformData(cmd, 1, sky_pose, sizeof sky_pose);
    SDL_DrawGPUPrimitives(pass, sky_vertices_, 1, track_vertices_, 0);
    for (unsigned i = 0; i < std::size(field); ++i) {
        const Slot &car = field[i];
        // Small surges and lane drift keep the pack breathing.
        const float s = lead - car.behind + 0.9f * std::sin(t * 0.7f + i * 1.9f);
        const float a = line.angle(s);
        const float offset = car.lane + 0.45f * std::sin(t * 0.37f + float(int(car.behind / 6)) * 0.8f);
        const Vec direction = tangent(a);
        auto paint = pigment(car.color);
        paint.w = direction.x;
        const Vec4 pose[] = {pack(lane(a, offset, 0.02f),direction.z),paint};
        SDL_PushGPUVertexUniformData(cmd, 1, pose, sizeof pose);
        SDL_DrawGPUPrimitives(pass, car_vertices_, 1, track_vertices_ + sky_vertices_, 0);
    }
    SDL_EndGPURenderPass(pass);
    ct.texture = swap;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, lens_);
    const SDL_GPUTextureSamplerBinding sampler{color_,sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
    const Vec4 lens{aspect,visibility,0,0};
    SDL_PushGPUFragmentUniformData(cmd, 0, &lens, sizeof lens);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

void HomeScene::shutdown() {
    if (!dev_) return;
    if (world_) SDL_ReleaseGPUGraphicsPipeline(dev_, world_);
    if (lens_) SDL_ReleaseGPUGraphicsPipeline(dev_, lens_);
    if (color_) SDL_ReleaseGPUTexture(dev_, color_);
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_);
    if (vertices_) SDL_ReleaseGPUBuffer(dev_, vertices_);
    dev_ = nullptr;
}

} // namespace app
