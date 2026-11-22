#pragma once

#include "runtime/checked_trace_file.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>

namespace app {

// An opt-in trace of submitted game source textures. A new rendered
// generation gets one download; presentations of it share its digest.
class HardwarePresentTrace {
public:
    HardwarePresentTrace(SDL_GPUDevice *device, Uint32 max_width, Uint32 height, const char *path)
        : device_(device), height_(height), pitch_((max_width + 63) & ~63u), file_(path) {
        if (std::fputs("present_ns,digest\n", file_.get()) < 0) {
            file_.close();
            throw std::runtime_error("cannot write hardware presentation trace header");
        }
        const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                   pitch_ * height_ * 4, 0};
        for (auto &slot : slots_) {
            slot.buffer = SDL_CreateGPUTransferBuffer(device_, &info);
            if (!slot.buffer) {
                release();
                file_.close();
                throw std::runtime_error(SDL_GetError());
            }
        }
    }

    ~HardwarePresentTrace() { release(); }

    bool submit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *image, bool eligible,
                bool rasterized, Uint32 window_width, Uint32 window_height, Uint32 source_width) {
        if (!poll()) {
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
        const bool resized = generation_ &&
            (window_width != window_width_ || window_height != window_height_ || source_width != width_);
        if (!eligible || resized) {
            if (generation_) end_reason_ = resized ? "image size changed" : "left eligible race";
            return SDL_SubmitGPUCommandBuffer(cmd) || fail("cannot submit command buffer");
        }
        if (samples_.size() == max_pending_samples || source_width == 0 || source_width > pitch_) {
            SDL_SubmitGPUCommandBuffer(cmd);
            return fail("invalid source size or pending sample limit reached");
        }

        if (rasterized || !generation_) {
            Slot *available = nullptr;
            for (auto &slot : slots_)
                if (!slot.fence) {
                    available = &slot;
                    break;
                }
            if (!available) {
                SDL_SubmitGPUCommandBuffer(cmd);
                return fail("readback ring saturated");
            }
            auto *copy = SDL_BeginGPUCopyPass(cmd);
            if (!copy) {
                SDL_SubmitGPUCommandBuffer(cmd);
                return fail("cannot begin source download");
            }
            const SDL_GPUTextureRegion region{image, 0, 0, 0, 0, 0, source_width, height_, 1};
            const SDL_GPUTextureTransferInfo transfer{available->buffer, 0, pitch_, height_};
            SDL_DownloadFromGPUTexture(copy, &region, &transfer);
            SDL_EndGPUCopyPass(copy);
            available->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            if (!available->fence) return fail("cannot submit source download");
            available->generation = ++generation_;
        } else if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            return fail("cannot submit command buffer");
        }

        const uint64_t now = width_ ? SDL_GetTicksNS() : log_clock("begin");
        if (now <= last_ns_ && width_) return fail("non-increasing submission timestamp");
        last_ns_ = now;
        width_ = source_width;
        window_width_ = window_width;
        window_height_ = window_height;
        samples_.push_back({now, generation_});
        return flush_ready();
    }

    bool ended() const { return end_reason_ != nullptr; }

    // Called after SDL_WaitForGPUIdle, before destroying the device.
    bool finish() {
        if (width_) log_clock("end");
        const bool complete = !error_ && poll() && samples_.empty() && written_ >= 2;
        const bool closed = file_.close();
        std::printf("daytona: hardware present trace samples=%llu generations=%llu (%s)\n",
                    static_cast<unsigned long long>(written_),
                    static_cast<unsigned long long>(generation_),
                    error_ ? error_ : end_reason_ ? end_reason_ : "run ended");
        return complete && closed;
    }

private:
    struct Slot {
        SDL_GPUTransferBuffer *buffer = nullptr;
        SDL_GPUFence *fence = nullptr;
        uint64_t generation = 0;
    };
    struct Sample {
        uint64_t timestamp, generation;
    };
    static constexpr size_t max_pending_samples = 256;

    // Bracket the relative SDL timestamp with native performance counters.
    // On Windows these counters use the same QPC clock as the ETW tools.
    static uint64_t log_clock(const char *stage) {
        const uint64_t before = SDL_GetPerformanceCounter();
        const uint64_t ticks_ns = SDL_GetTicksNS();
        const uint64_t after = SDL_GetPerformanceCounter();
        std::printf("daytona: present clock %s ticks_ns=%llu counter_min=%llu counter_max=%llu frequency=%llu\n",
                    stage, static_cast<unsigned long long>(ticks_ns),
                    static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
                    static_cast<unsigned long long>(SDL_GetPerformanceFrequency()));
        return ticks_ns;
    }

    bool fail(const char *message) {
        error_ = message;
        return false;
    }

    bool poll() {
        for (auto &slot : slots_) {
            if (!slot.fence || !SDL_QueryGPUFence(device_, slot.fence)) continue;
            const auto *data = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device_, slot.buffer, false));
            if (!data) return fail("cannot map completed source download");
            uint64_t hash = 0xcbf29ce484222325ULL;
            for (Uint32 row = 0; row < height_; ++row) {
                const auto *bytes = data + size_t(row) * pitch_ * 4;
                for (Uint32 byte = 0; byte < width_ * 4; ++byte) {
                    hash ^= bytes[byte];
                    hash *= 0x100000001b3ULL;
                }
            }
            SDL_UnmapGPUTransferBuffer(device_, slot.buffer);
            SDL_ReleaseGPUFence(device_, slot.fence);
            slot.fence = nullptr;
            digests_.emplace(slot.generation, hash);
        }
        return flush_ready();
    }

    bool flush_ready() {
        while (!samples_.empty()) {
            const Sample &sample = samples_.front();
            const auto digest = digests_.find(sample.generation);
            if (digest == digests_.end()) break;
            if (std::fprintf(file_.get(), "%llu,%ux%016llx\n",
                    static_cast<unsigned long long>(sample.timestamp), static_cast<unsigned>(width_),
                    static_cast<unsigned long long>(digest->second)) < 0)
                return fail("cannot write hardware presentation trace");
            samples_.pop_front();
            ++written_;
        }
        const auto oldest = samples_.empty() ? generation_ : samples_.front().generation;
        digests_.erase(digests_.begin(), digests_.lower_bound(oldest));
        return true;
    }

    void release() {
        for (auto &slot : slots_) {
            if (slot.fence) SDL_ReleaseGPUFence(device_, slot.fence);
            if (slot.buffer) SDL_ReleaseGPUTransferBuffer(device_, slot.buffer);
        }
    }

    SDL_GPUDevice *device_;
    Uint32 height_, pitch_;
    rt::CheckedTraceFile file_;
    std::array<Slot, 2> slots_{};
    std::deque<Sample> samples_;
    std::map<uint64_t, uint64_t> digests_;
    uint64_t generation_ = 0, last_ns_ = 0, written_ = 0;
    Uint32 width_ = 0, window_width_ = 0, window_height_ = 0;
    const char *error_ = nullptr;
    const char *end_reason_ = nullptr;
};

} // namespace app
