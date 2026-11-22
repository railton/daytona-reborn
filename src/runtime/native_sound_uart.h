// Sample-time FIFO for the i960 sound UART. The reference board
// (SoundBoard::send) spaces each byte by kIps * 10 / 31250 = 240
// instructions. At 48 kHz that interval is 720/47 frames. The line keeps
// this exact rational, matching max(line_free, now) + 240. A consumer sees
// the byte on the ceiling sample, so a completion inside a frame waits for
// the next one; an exact boundary stays on that sample. No MIDI parsing
// and no SDL. Fixed storage, no allocation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace snd {

class NativeSoundUart {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint64_t kIps = 752000;
    static constexpr uint64_t kByteInstructions = 240;
    // 240 * 48000 / 752000 = 720/47.
    static constexpr uint64_t kSpacingNumer = 720;
    static constexpr uint64_t kSpacingDenom = 47;
    static constexpr size_t kCapacity = 256;

    struct Arrival {
        uint8_t value = 0;
        uint64_t sample = 0;
    };

    // Accept the whole span or nothing. `origin` is the sample at which the
    // bytes are written and must be monotone (equal origins continue one
    // burst). A full queue or an unrepresentable line time rejects the call.
    bool enqueue(uint64_t origin, std::span<const uint8_t> bytes) noexcept;

    // The next byte whose arrival sample is <= now, in line order.
    bool peek(uint64_t now, Arrival &out) const noexcept;
    bool take(uint64_t now, Arrival &out) noexcept;

    // Drop queued bytes. The line stays busy until its exact free time.
    void clear() noexcept;
    // Drop queued bytes and return the line to idle. The next origin is free.
    void reset() noexcept;

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    static constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

    static bool next_at(uint64_t origin_ticks, uint64_t line, uint64_t &at) noexcept {
        const uint64_t start = line > origin_ticks ? line : origin_ticks;
        if (start > kMax - kSpacingNumer) return false;
        at = start + kSpacingNumer;
        return at <= kMax - (kSpacingDenom - 1);
    }

    std::array<Arrival, kCapacity> slots_{};
    size_t head_ = 0, size_ = 0;
    uint64_t line_ticks_ = 0, last_origin_ = 0;
    bool have_origin_ = false;
};

static_assert(NativeSoundUart::kByteInstructions * NativeSoundUart::kSampleRate / NativeSoundUart::kIps == 15);
static_assert(NativeSoundUart::kSpacingNumer * NativeSoundUart::kIps ==
              NativeSoundUart::kByteInstructions * NativeSoundUart::kSampleRate * NativeSoundUart::kSpacingDenom);
static_assert((NativeSoundUart::kSpacingNumer + NativeSoundUart::kSpacingDenom - 1) /
                  NativeSoundUart::kSpacingDenom ==
              16);

inline bool NativeSoundUart::enqueue(uint64_t origin, std::span<const uint8_t> bytes) noexcept {
    if (have_origin_ && origin < last_origin_) return false;
    if (bytes.size() > kCapacity - size_) return false;
    if (origin > kMax / kSpacingDenom) return false;
    const uint64_t origin_ticks = origin * kSpacingDenom;
    uint64_t line = line_ticks_;
    std::array<Arrival, kCapacity> staged{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint64_t at = 0;
        if (!next_at(origin_ticks, line, at)) return false;
        staged[i].value = bytes[i];
        staged[i].sample = (at + kSpacingDenom - 1) / kSpacingDenom;
        line = at;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        slots_[(head_ + size_) % kCapacity] = staged[i];
        ++size_;
    }
    line_ticks_ = line;
    last_origin_ = origin;
    have_origin_ = true;
    return true;
}

inline bool NativeSoundUart::peek(uint64_t now, Arrival &out) const noexcept {
    if (size_ == 0 || slots_[head_].sample > now) return false;
    out = slots_[head_];
    return true;
}

inline bool NativeSoundUart::take(uint64_t now, Arrival &out) noexcept {
    if (!peek(now, out)) return false;
    head_ = (head_ + 1) % kCapacity;
    --size_;
    return true;
}

inline void NativeSoundUart::clear() noexcept {
    head_ = 0;
    size_ = 0;
}

inline void NativeSoundUart::reset() noexcept {
    clear();
    line_ticks_ = 0;
    last_origin_ = 0;
    have_origin_ = false;
}

} // namespace snd
