// Automatic quality: what the graphics card can draw (app/quality.h picks
// the level). Each level's stand-in frame (GpuRenderer::draw_benchmark, in
// renderers of its own with the level's MSAA and texture filtering) is drawn
// at the level's resolution in batches, each batch waited for; the GPU time a
// frame is the median batch's. Every level is measured, the highest first.
// step() does one batch, a few milliseconds to a few tens, so that a window
// can show the progress between steps.
#pragma once

#include "app/gpu/gpu_renderer.h"
#include "app/quality.h"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace app {

class QualityProbe {
public:
    enum class Stage { Idle, Preparing, Maximum, High, Medium, Done, Failed };
    // display_w x display_h: the screen, in pixels. native_modes: the
    // screen's own resolution exists (Direct3D 12, Vulkan): Maximum is
    // measured at the screen's size, as the native mode draws; else, as High,
    // at 4 times the arcade's or the most the screen's height takes.
    void start(SDL_GPUDevice *dev, int display_w, int display_h, bool native_modes);
    void step();
    void stop(); // releases its renderers and target
    ~QualityProbe() { stop(); }
    bool running() const { return stage_ != Stage::Idle && stage_ != Stage::Done && stage_ != Stage::Failed; }
    Stage stage() const { return stage_; }
    float progress() const; // 0..1
    const QualityTimes &times() const { return times_; }
    QualityLevel level() const { return pick_quality(times_); }
    bool native_modes() const { return native_modes_; }
    const std::string &gpu_name() const { return gpu_name_; }
    const std::string &error() const { return error_; }
    // the resolution each level is measured at
    struct Size { int w, h; };
    Size size_of(Stage level) const;

private:
    // One plane over the whole frame. Calibrated against the game (RTX 5090,
    // Vulkan, 7680 x 4320, MSAA 4x, anisotropic 16x, every car option): the
    // stand-in takes 3.5 ms; a race frame, all its passes and the game's own
    // work included, at most 2.1 ms (m2gpushot --bench --pixels, frames 2400
    // to 3600 of the attract run). So the stand-in errs on the slow side.
    static constexpr int kLayers = 1;
    static constexpr int kFramesPerBatch = 6;
    static constexpr int kWarmBatches = 2, kBatches = 9;
    SDL_GPUDevice *dev_ = nullptr;
    Stage stage_ = Stage::Idle;
    int display_w_ = 0, display_h_ = 0;
    bool native_modes_ = false;
    std::unique_ptr<GpuRenderer> msaa4_, msaa2_;
    SDL_GPUTexture *target_ = nullptr;
    int target_w_ = 0, target_h_ = 0;
    int batch_ = 0;               // of the current level (warm batches first)
    std::vector<double> samples_; // ms a frame, per batch
    QualityTimes times_;
    std::string gpu_name_, error_;
    bool measure(GpuRenderer &r, Size size); // one batch; false on an error
    void finish_level(double ms);
    void fail(const std::string &why);
};

} // namespace app
