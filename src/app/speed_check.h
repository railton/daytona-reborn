// Automatic quality, in the race (app/quality.h): whether the computer kept
// up with the chosen image quality, and which part did not. Once a session,
// in a race with an image enhancement on, the frames the renderer draws are
// counted over a window (from kSkipNs after the race starts, for kWindowNs);
// fewer than kKeepUp of the game's rate means the frames were late. The time
// of the main loop is split meanwhile: the waits (the pacer's hold, the
// graphics card's swapchain: wait()) and the rest, the processor's own work
// (the game, the renderer's preparation, the custom car). When that work
// alone takes most of a frame, the processor is what held the race back;
// else the graphics card. The player is told which, with what to lower.
#pragma once

#include <cstdint>

namespace app {

class SpeedCheck {
public:
    static constexpr uint64_t kSkipNs = 3'000'000'000ull;    // the start's loading
    static constexpr uint64_t kWindowNs = 10'000'000'000ull; // then 10 s counted
    static constexpr double kKeepUp = 0.92;                 // of the game's rate
    // the processor held it back when its work takes this share of a frame at the game's rate
    static constexpr double kCpuShare = 0.8;

    enum class Limit { None, Processor, GraphicsCard };

    // Time spent waiting (the pacer, the swapchain), as it happens; counted in the window only.
    void wait(uint64_t ns) {
        if (counting_) waited_ += ns;
    }
    // Each pass of the main loop while racing: drew a new frame or not.
    // game_hz: the rate the game runs at. Returns true once, when the window
    // ends with the frames late (then slow(), fps(), limit() and cpu_ms() tell).
    bool pass(uint64_t now_ns, bool drew, double game_hz) {
        if (done_) return false;
        if (!start_) start_ = now_ns;
        if (now_ns < start_ + kSkipNs) return false;
        if (now_ns < start_ + kSkipNs + kWindowNs) {
            if (!counting_) counting_ = true, waited_ = 0;
            frames_ += drew ? 1 : 0;
            return false;
        }
        done_ = true;
        counting_ = false;
        fps_ = double(frames_) * 1e9 / double(kWindowNs);
        const uint64_t busy = waited_ < kWindowNs ? kWindowNs - waited_ : 0;
        cpu_ms_ = frames_ ? double(busy) / 1e6 / double(frames_) : 0;
        slow_ = fps_ < game_hz * kKeepUp;
        if (slow_) limit_ = cpu_ms_ >= kCpuShare * 1000.0 / game_hz ? Limit::Processor : Limit::GraphicsCard;
        return slow_;
    }
    // A race that ended (or paused) before the window: the next one starts afresh.
    void interrupt() {
        if (!done_) start_ = 0, frames_ = 0, waited_ = 0, counting_ = false;
    }
    bool done() const { return done_; }
    bool slow() const { return slow_; }
    double fps() const { return fps_; }
    Limit limit() const { return limit_; }
    // the processor's own work a drawn frame, in milliseconds
    double cpu_ms() const { return cpu_ms_; }

private:
    uint64_t start_ = 0, frames_ = 0, waited_ = 0;
    bool done_ = false, slow_ = false, counting_ = false;
    double fps_ = 0, cpu_ms_ = 0;
    Limit limit_ = Limit::None;
};

} // namespace app
