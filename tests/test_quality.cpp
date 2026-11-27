// Automatic quality (app/quality.h): the level picked from measured times,
// and each level's settings.
#include "app/quality.h"
#include "app/speed_check.h"

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
              cfg.aniso == 16 && cfg.smooth_cars && cfg.car_reflections && cfg.sharp_car_textures && cfg.car_shadows &&
              cfg.smooth_2d && cfg.scenery_3d && cfg.cars_near_model && cfg.pace_smooth,
          "maximum: native, MSAA 4x, 16x, every car option, smooth menus, 3D scenery, detailed opponents");
    check(cfg.bloom && cfg.crt_filter, "the screen effects are left as they were");
    app::apply_quality(cfg, QualityLevel::Maximum, false);
    check(cfg.supersampling == 4, "maximum without the native modes: 4 times");
    app::apply_quality(cfg, QualityLevel::Medium, true);
    check(cfg.supersampling == 2 && cfg.msaa == 2 && cfg.aniso == 4 && cfg.smooth_cars && !cfg.car_shadows &&
              !cfg.sharp_car_textures && !cfg.smooth_2d && !cfg.scenery_3d && !cfg.cars_near_model,
          "medium: 2x, MSAA 2x, 4x, the light cars' options");
    app::apply_quality(cfg, QualityLevel::High, true);
    check(cfg.smooth_2d && cfg.car_shadows && !cfg.scenery_3d && !cfg.cars_near_model, "high: smooth menus, not the processor's options");
    app::apply_quality(cfg, QualityLevel::Original, true);
    check(cfg.renderer == "hardware" && cfg.supersampling == 1 && cfg.msaa == 1 && cfg.aniso == 1 && !cfg.smooth_cars &&
              !cfg.car_reflections,
          "original: the enhancements off, the renderer kept");

    // the race's check: frames drawn over the window, against the game's rate
    // waiting: the share of each pass spent waiting (the pacer, the swapchain)
    auto run = [](double drawn_hz, double game_hz, app::SpeedCheck &c, double waiting = 0) {
        const uint64_t step = uint64_t(1e9 / 240.0); // passes at 240 a second, some drawing
        double owed = 0;
        bool told = false;
        for (uint64_t t = 1; t < 15'000'000'000ull; t += step) {
            owed += drawn_hz / 240.0;
            const bool drew = owed >= 1.0;
            if (drew) owed -= 1.0;
            c.wait(uint64_t(double(step) * waiting));
            told |= c.pass(t, drew, game_hz);
        }
        return told;
    };
    app::SpeedCheck fast, slow, interrupted;
    check(!run(57.5, 57.5, fast) && fast.done() && !fast.slow() && fast.fps() > 57.0, "full speed: nothing to tell");
    check(run(45.0, 57.5, slow) && slow.slow() && slow.fps() > 44.0 && slow.fps() < 46.0, "45 frames/s: told, once");
    // which part held it back: the processor busy all along, or waiting for the graphics card most of the time
    app::SpeedCheck cpu_bound, gpu_bound;
    check(run(45.0, 57.5, cpu_bound) && cpu_bound.limit() == app::SpeedCheck::Limit::Processor && cpu_bound.cpu_ms() > 21.0,
          "busy all along: the processor");
    check(run(45.0, 57.5, gpu_bound, 0.7) && gpu_bound.limit() == app::SpeedCheck::Limit::GraphicsCard &&
              gpu_bound.cpu_ms() > 6.0 && gpu_bound.cpu_ms() < 7.5,
          "waiting 70 % of the time: the graphics card");
    check(fast.limit() == app::SpeedCheck::Limit::None, "full speed: no limit named");
    interrupted.pass(1, true, 57.5);
    interrupted.pass(4'000'000'000ull, true, 57.5);
    interrupted.interrupt();
    check(!interrupted.done(), "a race cut short: measured again in the next");

    if (!failures) std::printf("quality: all checks passed\n");
    return failures ? 1 : 0;
}
