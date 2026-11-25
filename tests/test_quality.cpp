// Automatic quality (app/quality.h): the level picked from measured times,
// and each level's settings.
#include "app/quality.h"

#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char *message) {
        if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    using app::QualityLevel;
    using app::QualityTimes;
    using app::pick_quality;
    const double b = app::kQualityBudgetMs;
    check(pick_quality(QualityTimes{}) == QualityLevel::Original, "nothing measured: the original");
    check(pick_quality({b, -1, -1}) == QualityLevel::Maximum, "maximum at the budget fits");
    check(pick_quality({b + 0.01, -1, -1}) == QualityLevel::Original, "maximum over it, nothing else measured");
    check(pick_quality({b * 3, b * 0.5, -1}) == QualityLevel::High, "high when maximum is over");
    check(pick_quality({b * 9, b * 2, b * 0.9}) == QualityLevel::Medium, "medium when high is over");
    check(pick_quality({b * 9, b * 4, b * 1.5}) == QualityLevel::Original, "everything over: the original");
    check(pick_quality({0.0, b * 4, b * 1.5}) == QualityLevel::Maximum, "a level measured at 0 ms fits");

    app::Config cfg;
    cfg.renderer = "software";
    cfg.bloom = cfg.crt_filter = true;
    app::apply_quality(cfg, QualityLevel::Maximum, true);
    check(cfg.renderer == "hardware" && cfg.supersampling == app::Config::kNativeResolution && cfg.msaa == 4 &&
              cfg.aniso == 16 && cfg.smooth_cars && cfg.car_reflections && cfg.sharp_car_textures && cfg.car_shadows,
          "maximum: native, MSAA 4x, 16x, every car option");
    check(cfg.bloom && cfg.crt_filter, "the screen effects are left as they were");
    app::apply_quality(cfg, QualityLevel::Maximum, false);
    check(cfg.supersampling == 4, "maximum without the native modes: 4 times");
    app::apply_quality(cfg, QualityLevel::Medium, true);
    check(cfg.supersampling == 2 && cfg.msaa == 2 && cfg.aniso == 4 && cfg.smooth_cars && !cfg.car_shadows &&
              !cfg.sharp_car_textures,
          "medium: 2x, MSAA 2x, 4x, the light cars' options");
    app::apply_quality(cfg, QualityLevel::Original, true);
    check(cfg.renderer == "hardware" && cfg.supersampling == 1 && cfg.msaa == 1 && cfg.aniso == 1 && !cfg.smooth_cars &&
              !cfg.car_reflections,
          "original: the enhancements off, the renderer kept");

    if (!failures) std::printf("quality: all checks passed\n");
    return failures ? 1 : 0;
}
