// Automatic quality (Settings > Enhancements > Image quality, and once at the
// first start): the level a graphics card's measured times recommend, and
// the settings of each level. The measurement is app/gpu/quality_probe.h.
#pragma once

#include "app/config.h"

#include <string>
#include <utility>

namespace app {

enum class QualityLevel { Original, Medium, High, Maximum };

// The arcade's raster (496 x 384) enlarged `requested` times (1..4), but
// never past the screen: the largest factor whose height is at most the
// screen's (10 % over allowed: 3x, 1152 lines, on a 1080-line screen).
// Drawing more lines than the screen shows only costs the graphics card.
constexpr int kArcadeHeight = 384, kArcadeWidth = 496;
constexpr int arcade_scale_for(int requested, int screen_height) {
    int fit = 1;
    while (fit < 4 && kArcadeHeight * (fit + 1) * 10 <= screen_height * 11) ++fit;
    const int want = requested < 1 ? 1 : requested > 4 ? 4 : requested;
    return screen_height > 0 && want > fit ? fit : want;
}

// GPU milliseconds a frame of each level's stand-in (GpuRenderer::draw_benchmark), < 0 not measured.
struct QualityTimes {
    double maximum = -1, high = -1, medium = -1;
};

// A level fits when its stand-in takes at most half a 60 Hz frame: the rest
// is for the parts the stand-in leaves out (the tilemaps, the cars' passes,
// the display passes, the interface) and a margin.
constexpr double kQualityBudgetMs = 8.0;

// The highest level measured within the budget; Original if none.
constexpr QualityLevel pick_quality(const QualityTimes &t) {
    auto fits = [](double ms) { return ms >= 0 && ms <= kQualityBudgetMs; };
    if (fits(t.maximum)) return QualityLevel::Maximum;
    if (fits(t.high)) return QualityLevel::High;
    if (fits(t.medium)) return QualityLevel::Medium;
    return QualityLevel::Original;
}

// The settings of a level. native_modes: the screen's own resolution exists
// (Direct3D 12, Vulkan); else Maximum draws at 4 times the arcade's. The
// arcade multiples are drawn at most at the screen's size (arcade_scale_for).
// Original turns the image enhancements off; the renderer stays as it is.
// High and Maximum add the smooth menus and HUD (0.03 ms measured), Maximum
// the 3D scenery and the always-detailed opponents (a recent processor:
// +0.15 and +0.38 ms at the 99th percentile measured, 27/11; they apply at
// the next start). The display's smooth pacing is set (it acts only on a
// 57.52 or 115.05 Hz display). The screen effects (bloom, CRT) are a matter
// of taste: never touched.
inline void apply_quality(Config &cfg, QualityLevel level, bool native_modes) {
    const bool on = level != QualityLevel::Original;
    if (on) cfg.renderer = "hardware";
    switch (level) {
    case QualityLevel::Maximum:
        cfg.supersampling = native_modes ? Config::kNativeResolution : 4;
        cfg.msaa = 4, cfg.aniso = 16;
        break;
    case QualityLevel::High:
        cfg.supersampling = 4, cfg.msaa = 4, cfg.aniso = 16;
        break;
    case QualityLevel::Medium:
        cfg.supersampling = 2, cfg.msaa = 2, cfg.aniso = 4;
        break;
    case QualityLevel::Original:
        cfg.supersampling = 1, cfg.msaa = 1, cfg.aniso = 1;
        break;
    }
    const bool high = level == QualityLevel::High || level == QualityLevel::Maximum;
    cfg.smooth_cars = cfg.car_reflections = on;
    cfg.sharp_car_textures = cfg.car_shadows = cfg.smooth_2d = high;
    cfg.scenery_3d = cfg.cars_near_model = level == QualityLevel::Maximum;
    cfg.pace_smooth = true;
}

// What a level set, for the log.
inline std::string quality_summary(const Config &cfg) {
    std::string s = "renderer " + cfg.renderer + ", resolution " +
                    (cfg.uses_native_resolution() ? std::string("native") : std::to_string(cfg.supersampling) + "x") + ", MSAA " +
                    std::to_string(cfg.msaa) + "x, anisotropic " + std::to_string(cfg.aniso) + "x";
    const std::pair<bool, const char *> options[] = {
        {cfg.smooth_cars, "smooth car shading"}, {cfg.car_reflections, "paint reflections"},
        {cfg.sharp_car_textures, "sharp car textures"}, {cfg.car_shadows, "car shadows"}, {cfg.smooth_2d, "smooth menus and HUD"},
        {cfg.scenery_3d, "3D scenery"}, {cfg.cars_near_model, "detailed opponents"}, {cfg.pace_smooth, "smooth pacing"}};
    for (const auto &[on, name] : options)
        if (on) s += std::string(", ") + name;
    return s;
}

} // namespace app
