#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace rt {

// Column-major affine matrix: three basis columns, then translation.
using BackgroundTransform = std::array<double, 12>;

struct BackgroundObject {
    // Polygon ROM address, link count and geometry mode.
    std::array<uint32_t, 3> geometry;
    BackgroundTransform model_view;
};

struct BackgroundMotion {
    BackgroundTransform transform;
    std::size_t support;
    std::size_t common_objects;
    double rotation_degrees;
    bool cut_candidate(double threshold = 30) const { return rotation_degrees >= threshold; }
};

namespace background_detail {

inline BackgroundTransform compose(const BackgroundTransform &a, const BackgroundTransform &b) {
    BackgroundTransform c;
    for (unsigned col = 0; col < 4; ++col) {
        for (unsigned row = 0; row < 3; ++row) {
            c[3*col + row] = a[row]*b[3*col] + a[3 + row]*b[3*col + 1] +
                             a[6 + row]*b[3*col + 2];
            if (col == 3) c[3*col + row] += a[9 + row];
        }
    }
    return c;
}

// The observed model-view matrices may carry scale. The inverse in the
// rigid pose interpolator is therefore not suitable for this fit.
inline std::optional<BackgroundTransform> affine_inverse(const BackgroundTransform &a) {
    if (!std::ranges::all_of(a, [](double v) { return std::isfinite(v); }) ||
        std::abs(a[11]) <= 0.01) return {};
    BackgroundTransform inverse{};
    for (unsigned col = 0; col < 3; ++col) {
        const unsigned j = (col + 1) % 3, k = (col + 2) % 3;
        for (unsigned row = 0; row < 3; ++row) {
            const unsigned i1 = (row + 1) % 3, i2 = (row + 2) % 3;
            inverse[3*row + col] = a[3*j + i1]*a[3*k + i2] - a[3*j + i2]*a[3*k + i1];
        }
    }
    const double det = a[0]*inverse[0] + a[3]*inverse[1] + a[6]*inverse[2];
    double scale = 0;
    for (unsigned i = 0; i < 9; ++i) scale = std::max(scale, std::abs(a[i]));
    if (std::abs(det) <= std::numeric_limits<double>::epsilon()*scale*scale*scale) return {};
    for (unsigned i = 0; i < 9; ++i) inverse[i] /= det;
    for (unsigned row = 0; row < 3; ++row) {
        inverse[9 + row] = -(inverse[row]*a[9] + inverse[3 + row]*a[10] + inverse[6 + row]*a[11]);
    }
    if (!std::ranges::all_of(inverse, [](double v) { return std::isfinite(v); })) return {};
    return inverse;
}

struct MatchableObject {
    const BackgroundTransform *matrix;
    BackgroundTransform inverse;
};

inline auto unique_objects(std::span<const BackgroundObject> objects) {
    std::map<std::array<uint32_t, 3>, MatchableObject> by_geometry;
    for (const auto &object : objects) {
        if (const auto inverse = affine_inverse(object.model_view)) {
            auto [it, inserted] = by_geometry.try_emplace(object.geometry, &object.model_view, *inverse);
            if (!inserted) it->second.matrix = nullptr;
        }
    }
    return by_geometry;
}

inline bool agrees(const BackgroundTransform &predicted, const BackgroundTransform &observed) {
    for (unsigned i = 0; i < 9; ++i) {
        if (!(std::abs(predicted[i] - observed[i]) <= 0.001)) return false;
    }
    const double x = predicted[9] - observed[9];
    const double y = predicted[10] - observed[10];
    const double z = predicted[11] - observed[11];
    return x*x + y*y + z*z <= 0.05*0.05;
}

} // namespace background_detail

// Fit the C6c reference over adjacent draws. The caller checks frame
// continuity and excludes identified mobile objects before fitting.
inline std::optional<BackgroundMotion> fit_background_motion(
    std::span<const BackgroundObject> previous, std::span<const BackgroundObject> current)
{
    using namespace background_detail;
    const auto left = unique_objects(previous);
    const auto right = unique_objects(current);
    struct Pair {
        const MatchableObject &before;
        const BackgroundTransform &after;
    };
    std::vector<Pair> pairs;
    for (const auto &[key, before] : left) {
        const auto next = right.find(key);
        if (before.matrix && next != right.end() && next->second.matrix)
            pairs.push_back({before, *next->second.matrix});
    }
    if (pairs.size() < 3) return {};

    BackgroundMotion best{{}, 0, pairs.size(), 0};
    for (const auto &seed : pairs) {
        const auto candidate = compose(seed.after, seed.before.inverse);
        std::size_t support = 0;
        for (const auto &pair : pairs)
            support += agrees(compose(candidate, *pair.before.matrix), pair.after);
        // Keep the first maximum in geometry-key order so ties are stable.
        if (support > best.support) {
            best.transform = candidate;
            best.support = support;
        }
    }
    if (best.support < 3) return {};
    const auto &d = best.transform;
    constexpr double degrees_per_radian = 57.2957795130823208768;
    best.rotation_degrees = std::acos(std::clamp((d[0] + d[4] + d[8] - 1)*0.5, -1.0, 1.0)) *
                            degrees_per_radian;
    return best;
}

} // namespace rt
