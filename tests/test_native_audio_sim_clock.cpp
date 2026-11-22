#include "../src/app/native_audio_sim_clock.h"

#include <array>
#include <cstdint>
#include <optional>

namespace {

using app::NativeAudioSimClock;
using std::uint64_t;

int failures = 0;

void check(bool ok) {
    if (!ok) {
        ++failures;
    }
}

void expect(const NativeAudioSimClock& clock, uint64_t tick, uint64_t frame) {
    const auto got = clock.frame_for(tick);
    check(got.has_value() && *got == frame);
}

void expect_none(const NativeAudioSimClock& clock, uint64_t tick) {
    check(!clock.frame_for(tick).has_value());
}

}  // namespace

int main() {
    // Nonzero origin: epoch start maps to origin_sample; earlier ticks are invalid.
    NativeAudioSimClock clock{1'000'000, 7'000};
    expect(clock, 1'000'000, 7'000);
    expect_none(clock, 999'999);

    // Exact 125-tick boundary and neighbours.
    expect(clock, 1'000'124, 110'469);
    expect(clock, 1'000'125, 111'304);
    expect(clock, 1'000'250, 215'608);

    // Caller reset to a new nonzero epoch.
    clock = NativeAudioSimClock{42, 99};
    expect(clock, 42, 99);
    expect(clock, 167, 104'403);
    expect_none(clock, 41);

    // Burst calls and individual calls at the same absolute stamps agree.
    constexpr uint64_t kEpochTick = 500'000;
    constexpr uint64_t kEpochSample = 9'000;
    constexpr std::array<uint64_t, 6> kTicks{
        kEpochTick, kEpochTick + 1, kEpochTick + 124,
        kEpochTick + 125, kEpochTick + 126, kEpochTick + 1'000'000};
    NativeAudioSimClock burst{kEpochTick, kEpochSample};
    for (const uint64_t tick : kTicks) {
        const NativeAudioSimClock individual{kEpochTick, kEpochSample};
        const auto from_burst = burst.frame_for(tick);
        const auto from_individual = individual.frame_for(tick);
        check(from_burst.has_value() && from_individual.has_value());
        if (from_burst && from_individual) {
            check(*from_burst == *from_individual);
        }
    }

    // Largest representable zero-origin mapping, then the first overflowing tick.
    constexpr uint64_t kLargestSafeTick = 22'106'947'089'408'785ULL;
    constexpr uint64_t kLargestSafeFrame = 18'446'744'073'709'551'285ULL;
    constexpr uint64_t kMax = kLargestSafeFrame + 330;
    const NativeAudioSimClock zero{0, 0};
    expect(zero, kLargestSafeTick, kLargestSafeFrame);
    expect_none(zero, kLargestSafeTick + 1);

    // Large representable value: naive 10^16 * 104304 would overflow uint64_t.
    expect(zero, 10'000'000'000'000'000ULL, 8'344'320'000'000'000'000ULL);

    // Addition of the origin sample overflows even though the offset fits.
    const NativeAudioSimClock top{1'000, kMax};
    expect(top, 1'000, kMax);
    expect_none(top, 1'125);

    return failures == 0 ? 0 : 1;
}
