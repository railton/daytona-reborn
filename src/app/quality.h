// Automatic quality (Settings > Enhancements > Image quality, and once at the
// first start): the level a graphics card's measured times recommend, and
// the settings of each level. The measurement is app/gpu/quality_probe.h.
#pragma once

#include "app/config.h"

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
// The screen effects (bloom, CRT) are a matter of taste: never touched.
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
    cfg.smooth_cars = cfg.car_reflections = on;
    cfg.sharp_car_textures = cfg.car_shadows = level == QualityLevel::High || level == QualityLevel::Maximum;
}

} // namespace app
