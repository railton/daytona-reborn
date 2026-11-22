// Bounded single-producer single-consumer queue of timestamped audio packets.
// One producer calls try_publish. One consumer calls peek and try_accept,
// or try_consume to accept the slot in place. A packet becomes visible only
// after its origin and payload are stored, by one release. A rejected
// publish, a too-small accept buffer, or a consume that returns false or
// throws does not drop or shorten any queued packet.
//
// The shared counters are uint32_t and lock-free. Each role also keeps its
// own slot cursor, advanced by one slot and wrapped at kSlotCount. The
// cursor is not counter % kSlotCount: for a capacity that does not divide
// the counter modulus, that expression repeats a slot when the counter
// returns to zero (uint32 max and 0 are both 0 mod 3). Occupancy stays the
// unsigned counter difference, so the capacity must fit in the counter.
//
// reset() is not concurrent: both sides must be stopped, as they must be
// before destruction. size() is a statistic: exact while both roles are
// stopped, and kept inside [0, capacity] if a concurrent observer samples
// the two counters. No SDL and no engine.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace app {

template <size_t kSlotCount, size_t kMaxBytes>
class NativeAudioPacketQueue {
public:
    static constexpr size_t kCapacity = kSlotCount;
    static constexpr size_t kPayloadLimit = kMaxBytes;
    static_assert(kSlotCount >= 1, "the queue needs at least one slot");
    static_assert(kSlotCount <= std::numeric_limits<uint32_t>::max(),
                  "occupancy must fit in the lock-free counter");
    static_assert(kMaxBytes <= 0xffffffffu, "payload length is stored in 32 bits");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "native audio requires lock-free counters");

    struct Header {
        uint64_t origin = 0;
        uint32_t size = 0;
    };

    // Store one whole packet, or leave the queue unchanged.
    bool try_publish(uint64_t origin, std::span<const uint8_t> payload) noexcept;

    // The next packet's origin and size. Does not remove it.
    bool peek(Header &out) const noexcept;

    // Copy the next packet into dest and remove it. A short dest refuses
    // the packet and leaves it at the head.
    bool try_accept(std::span<uint8_t> dest, Header &out) noexcept;

    // Call `accept` with the head packet. It receives the header and a const
    // span of that slot's payload. The span is valid only during the call
    // and must not be kept. `accept` must not reenter this queue. The slot
    // is released only when `accept` returns true. false, or an exception,
    // leaves the same packet at the head and the counters unchanged. The
    // exception propagates.
    template <class Accept>
    bool try_consume(Accept &&accept);

    // Both sides are stopped. The queue is empty afterwards.
    void reset() noexcept;

#ifdef NATIVE_AUDIO_PACKET_TESTING
    // Test hook, absent from application builds. Both sides are stopped and
    // the queue is empty. Moves the monotonic counters to `count` and leaves
    // the slot cursors where they are.
    bool stage_empty(uint32_t count) noexcept;
#endif

    // Freed is loaded before published. A torn pair is clamped into
    // [0, capacity]. With both roles stopped the difference is the occupancy.
    size_t size() const noexcept;

private:
    struct Slot {
        uint64_t origin = 0;
        uint32_t size = 0;
        std::array<uint8_t, kMaxBytes> bytes{};
    };
    struct Producer {
        std::atomic<uint32_t> published{0};
        uint32_t count = 0;
        uint32_t slot = 0;
    };
    struct Consumer {
        std::atomic<uint32_t> freed{0};
        uint32_t count = 0;
        uint32_t slot = 0;
    };

    static uint32_t advance(uint32_t slot) noexcept {
        return slot + 1 == kSlotCount ? 0 : slot + 1;
    }

    // Producer and consumer each own one line.
    alignas(64) Producer producer_;
    alignas(64) Consumer consumer_;
    std::array<Slot, kSlotCount> slots_{};
};

template <size_t kSlotCount, size_t kMaxBytes>
bool NativeAudioPacketQueue<kSlotCount, kMaxBytes>::try_publish(
    uint64_t origin, std::span<const uint8_t> payload) noexcept {
    if (payload.size() > kMaxBytes) return false;
    if (!payload.empty() && payload.data() == nullptr) return false;
    const uint32_t freed = consumer_.freed.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(producer_.count - freed) >= kSlotCount) return false;
    Slot &slot = slots_[producer_.slot];
    slot.origin = origin;
    slot.size = static_cast<uint32_t>(payload.size());
    if (!payload.empty()) std::memcpy(slot.bytes.data(), payload.data(), payload.size());
    producer_.slot = advance(producer_.slot);
    ++producer_.count;
    producer_.published.store(producer_.count, std::memory_order_release);
    return true;
}

template <size_t kSlotCount, size_t kMaxBytes>
bool NativeAudioPacketQueue<kSlotCount, kMaxBytes>::peek(Header &out) const noexcept {
    const uint32_t published = producer_.published.load(std::memory_order_acquire);
    if (consumer_.count == published) return false;
    const Slot &slot = slots_[consumer_.slot];
    out.origin = slot.origin;
    out.size = slot.size;
    return true;
}

template <size_t kSlotCount, size_t kMaxBytes>
template <class Accept>
bool NativeAudioPacketQueue<kSlotCount, kMaxBytes>::try_consume(Accept &&accept) {
    const uint32_t published = producer_.published.load(std::memory_order_acquire);
    if (consumer_.count == published) return false;
    const Slot &slot = slots_[consumer_.slot];
    const Header head{slot.origin, slot.size};
    const std::span<const uint8_t> payload(slot.bytes.data(), slot.size);
    if (!accept(head, payload)) return false;
    consumer_.slot = advance(consumer_.slot);
    ++consumer_.count;
    consumer_.freed.store(consumer_.count, std::memory_order_release);
    return true;
}

template <size_t kSlotCount, size_t kMaxBytes>
bool NativeAudioPacketQueue<kSlotCount, kMaxBytes>::try_accept(
    std::span<uint8_t> dest, Header &out) noexcept {
    return try_consume([&](Header head, std::span<const uint8_t> payload) {
        if (dest.size() < head.size) return false;
        if (head.size != 0 && dest.data() == nullptr) return false;
        out = head;
        if (head.size != 0) std::memcpy(dest.data(), payload.data(), head.size);
        return true;
    });
}

template <size_t kSlotCount, size_t kMaxBytes>
void NativeAudioPacketQueue<kSlotCount, kMaxBytes>::reset() noexcept {
    producer_.count = 0;
    producer_.slot = 0;
    producer_.published.store(0, std::memory_order_relaxed);
    consumer_.count = 0;
    consumer_.slot = 0;
    consumer_.freed.store(0, std::memory_order_relaxed);
}

#ifdef NATIVE_AUDIO_PACKET_TESTING
template <size_t kSlotCount, size_t kMaxBytes>
bool NativeAudioPacketQueue<kSlotCount, kMaxBytes>::stage_empty(uint32_t count) noexcept {
    if (producer_.count != consumer_.count) return false;
    if (producer_.published.load(std::memory_order_relaxed) != producer_.count) return false;
    if (consumer_.freed.load(std::memory_order_relaxed) != consumer_.count) return false;
    producer_.count = count;
    consumer_.count = count;
    producer_.published.store(count, std::memory_order_relaxed);
    consumer_.freed.store(count, std::memory_order_relaxed);
    return true;
}
#endif

template <size_t kSlotCount, size_t kMaxBytes>
size_t NativeAudioPacketQueue<kSlotCount, kMaxBytes>::size() const noexcept {
    const uint32_t freed = consumer_.freed.load(std::memory_order_acquire);
    const uint32_t published = producer_.published.load(std::memory_order_acquire);
    const uint32_t queued = static_cast<uint32_t>(published - freed);
    if (queued > kSlotCount) return kSlotCount;
    return queued;
}

}  // namespace app
