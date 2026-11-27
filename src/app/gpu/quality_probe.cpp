#include "app/gpu/quality_probe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace app {

void QualityProbe::start(SDL_GPUDevice *dev, int display_w, int display_h, bool native_modes) {
    stop();
    dev_ = dev;
    display_w_ = std::max(display_w, 640), display_h_ = std::max(display_h, 480);
    native_modes_ = native_modes;
    times_ = QualityTimes{};
    error_.clear();
    gpu_name_.clear();
    if (const char *name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(dev), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr))
        gpu_name_ = name;
    batch_ = 0;
    samples_.clear();
    stage_ = Stage::Preparing;
}

void QualityProbe::stop() {
    msaa4_.reset();
    msaa2_.reset();
    if (target_ && dev_) SDL_ReleaseGPUTexture(dev_, target_);
    target_ = nullptr;
    target_w_ = target_h_ = 0;
    if (running()) stage_ = Stage::Idle;
}

QualityProbe::Size QualityProbe::size_of(Stage level) const {
    switch (level) {
    // never past the screen: the native mode is the screen's own pixels, the
    // arcade multiples at most its height (app/quality.h, arcade_scale_for)
    case Stage::Maximum:
        if (native_modes_) return {display_w_, display_h_};
        [[fallthrough]];
    case Stage::High: {
        const int k = arcade_scale_for(4, display_h_);
        return {kArcadeWidth * k, kArcadeHeight * k};
    }
    case Stage::Medium: {
        const int k = arcade_scale_for(2, display_h_);
        return {kArcadeWidth * k, kArcadeHeight * k};
    }
    default: return {0, 0};
    }
}

float QualityProbe::progress() const {
    // Preparing, then up to three levels of kWarmBatches + kBatches each
    constexpr float per_level = float(kWarmBatches + kBatches);
    const float done = stage_ == Stage::Preparing ? 0.f
                     : stage_ == Stage::Maximum   ? 1.f + float(batch_)
                     : stage_ == Stage::High      ? 1.f + per_level + float(batch_)
                     : stage_ == Stage::Medium    ? 1.f + 2 * per_level + float(batch_)
                                                  : 1.f + 3 * per_level;
    return std::clamp(done / (1.f + 3 * per_level), 0.f, 1.f);
}

void QualityProbe::fail(const std::string &why) {
    error_ = why;
    msaa4_.reset();
    msaa2_.reset();
    stage_ = Stage::Failed;
}

bool QualityProbe::measure(GpuRenderer &r, Size size) {
    if (!target_ || size.w > target_w_ || size.h > target_h_) {
        if (target_) SDL_ReleaseGPUTexture(dev_, target_);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.width = Uint32(size.w), ti.height = Uint32(size.h);
        ti.layer_count_or_depth = ti.num_levels = 1;
        target_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!target_) return false;
        target_w_ = size.w, target_h_ = size.h;
    }
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev_);
    if (!cmd) return false;
    for (int f = 0; f < kFramesPerBatch; ++f)
        if (!r.draw_benchmark(cmd, target_, size.w, size.h, kLayers)) {
            SDL_CancelGPUCommandBuffer(cmd);
            return false;
        }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return false;
    SDL_WaitForGPUFences(dev_, true, &fence, 1);
    SDL_ReleaseGPUFence(dev_, fence);
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / kFramesPerBatch;
    if (batch_ >= kWarmBatches) samples_.push_back(ms);
    ++batch_;
    return true;
}

void QualityProbe::finish_level(double ms) {
    batch_ = 0;
    samples_.clear();
    switch (stage_) { // every level, so that each one's time can be shown
    case Stage::Maximum: times_.maximum = ms, stage_ = Stage::High; break;
    case Stage::High: times_.high = ms, stage_ = Stage::Medium; break;
    default: times_.medium = ms, stage_ = Stage::Done; break;
    }
    if (stage_ == Stage::Done) {
        // for a log sent with a problem: what was measured, and the level it gives
        const char *names[] = {"original", "medium", "high", "maximum"};
        const auto mx = size_of(Stage::Maximum), hi = size_of(Stage::High), md = size_of(Stage::Medium);
        std::printf("daytona: image quality test on %s (screen %dx%d): maximum %.1f ms at %dx%d, high %.1f ms at %dx%d, "
                    "medium %.1f ms at %dx%d (budget %.0f ms): recommended %s\n",
                    gpu_name_.empty() ? "the graphics card" : gpu_name_.c_str(), display_w_, display_h_, times_.maximum, mx.w, mx.h,
                    times_.high, hi.w, hi.h, times_.medium, md.w, md.h, kQualityBudgetMs, names[int(level())]);
        stop();
    }
}

void QualityProbe::step() {
    if (!running()) return;
    if (stage_ == Stage::Preparing) {
        msaa4_ = std::make_unique<GpuRenderer>();
        if (!msaa4_->init(dev_, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 4)) return fail(msaa4_->error());
        msaa4_->set_anisotropy(16);
        stage_ = Stage::Maximum;
        return;
    }
    GpuRenderer *r = msaa4_.get();
    if (stage_ == Stage::Medium) {
        if (!msaa2_) {
            msaa2_ = std::make_unique<GpuRenderer>();
            if (!msaa2_->init(dev_, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 2)) return fail(msaa2_->error());
            msaa2_->set_anisotropy(4);
        }
        r = msaa2_.get();
    }
    if (!measure(*r, size_of(stage_))) return fail(SDL_GetError());
    if (batch_ < kWarmBatches + kBatches) return;
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    finish_level(sorted[sorted.size() / 2]);
}

} // namespace app
