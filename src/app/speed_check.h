// Automatic quality, in the race (app/quality.h): whether the graphics card
// kept up with the chosen image quality. Once a session, in a race with an
// image enhancement on, the frames the renderer draws are counted over a
// window (from kSkipNs after the race starts, for kWindowNs); fewer than
// kKeepUp of the game's rate means the frames were late, and the player is
// told that a lower quality may help.
#pragma once

#include <cstdint>

namespace app {

class SpeedCheck {
public:
    static constexpr uint64_t kSkipNs = 3'000'000'000ull;    // the start's loading
    static constexpr uint64_t kWindowNs = 10'000'000'000ull; // then 10 s counted
    static constexpr double kKeepUp = 0.92;                 // of the game's rate

    // Each pass of the main loop while racing: drew a new frame or not.
    // game_hz: the rate the game runs at. Returns true once, when the window
    // ends with the frames late (then slow() and fps() tell).
    bool pass(uint64_t now_ns, bool drew, double game_hz) {
        if (done_) return false;
        if (!start_) start_ = now_ns;
        if (now_ns < start_ + kSkipNs) return false;
        if (now_ns < start_ + kSkipNs + kWindowNs) {
            frames_ += drew ? 1 : 0;
            return false;
        }
        done_ = true;
        fps_ = double(frames_) * 1e9 / double(kWindowNs);
        slow_ = fps_ < game_hz * kKeepUp;
        return slow_;
    }
    // A race that ended (or paused) before the window: the next one starts afresh.
    void interrupt() {
        if (!done_) start_ = 0, frames_ = 0;
    }
    bool done() const { return done_; }
    bool slow() const { return slow_; }
    double fps() const { return fps_; }

private:
    uint64_t start_ = 0, frames_ = 0;
    bool done_ = false, slow_ = false;
    double fps_ = 0;
};

} // namespace app
