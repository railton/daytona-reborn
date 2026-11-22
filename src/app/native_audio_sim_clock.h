#ifndef APP_NATIVE_AUDIO_SIM_CLOCK_H
#define APP_NATIVE_AUDIO_SIM_CLOCK_H

#include <cstdint>
#include <limits>
#include <optional>

namespace app {

struct NativeAudioSimClock {
    std::uint64_t origin_tick = 0;
    std::uint64_t origin_sample = 0;

    [[nodiscard]] std::optional<std::uint64_t>
    frame_for(std::uint64_t simulation_tick) const noexcept {
        if (simulation_tick < origin_tick) {
            return std::nullopt;
        }

        constexpr std::uint64_t kNumerator = 104304;
        constexpr std::uint64_t kDenominator = 125;
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

        const std::uint64_t delta = simulation_tick - origin_tick;
        const std::uint64_t whole = delta / kDenominator;
        const std::uint64_t remainder = delta % kDenominator;

        if (whole > kMax / kNumerator) {
            return std::nullopt;
        }

        const std::uint64_t base = whole * kNumerator;
        const std::uint64_t tail = (remainder * kNumerator) / kDenominator;
        if (base > kMax - tail) {
            return std::nullopt;
        }

        const std::uint64_t offset = base + tail;
        if (origin_sample > kMax - offset) {
            return std::nullopt;
        }

        return origin_sample + offset;
    }
};

}  // namespace app

#endif  // APP_NATIVE_AUDIO_SIM_CLOCK_H
