// Synthetic UART FIFO. No ROM, no SDL, no sequencer. Known arrivals are the
// ceiling of n * 720/47 frames from an integer sample origin.
#include "runtime/native_sound_uart.h"

#include <cstdio>
#include <cstdint>
#include <span>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using Uart = snd::NativeSoundUart;
struct Item {
    uint8_t value;
    uint64_t sample;
    bool operator==(const Item &other) const { return value == other.value && sample == other.sample; }
};

std::vector<Item> drain(Uart &uart, uint64_t now) {
    std::vector<Item> out;
    Uart::Arrival arrival;
    while (uart.take(now, arrival)) out.push_back({arrival.value, arrival.sample});
    return out;
}

std::vector<uint8_t> bytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = uint8_t(seed + i);
    return out;
}
} // namespace

int main() {
    // Origin 0. Byte k completes at tick (k+1)*720, sample ceil of that / 47.
    // 720/47, 1440/47, 2160/47, 2880/47 and the 47th byte at 47*720/47 = 720.
    {
        Uart uart;
        const auto packet = bytes(47, 1);
        check(uart.enqueue(0, packet), "first burst fits");
        const auto got = drain(uart, 720);
        check(got.size() == 47, "47 bytes have arrived by sample 720");
        const uint64_t known[] = {16, 31, 46, 62};
        bool opening = got.size() == 47;
        for (size_t i = 0; opening && i < 4; ++i)
            opening &= got[i].sample == known[i] && got[i].value == packet[i];
        check(opening, "opening arrivals are 16, 31, 46, 62 in order");
        check(opening && got[46].sample == 720 && got[46].value == packet[46], "exact boundary stays on sample 720");
        bool ordered = opening;
        for (size_t i = 1; ordered && i < got.size(); ++i)
            ordered &= got[i].sample > got[i - 1].sample && got[i].value == packet[i];
        check(ordered, "arrivals stay strictly ordered");
        check(uart.empty(), "the burst is consumed");
    }

    // A free line at sample 5 delivers on 5 + 16. The same four bytes, in
    // one packet or two, share arrivals. A later origin still inside the
    // busy line does too.
    {
        const uint8_t all[] = {10, 20, 30, 40};
        Uart grouped, split, later;
        check(grouped.enqueue(5, all), "grouped enqueue");
        check(split.enqueue(5, std::span<const uint8_t>(all, 2)), "split head");
        check(split.enqueue(5, std::span<const uint8_t>(all + 2, 2)), "split tail at the same origin");
        check(later.enqueue(5, std::span<const uint8_t>(all, 2)), "later head");
        check(later.enqueue(10, std::span<const uint8_t>(all + 2, 2)), "later tail while the line is busy");
        const auto a = drain(grouped, 1000), b = drain(split, 1000), c = drain(later, 1000);
        check(a.size() == 4 && a == b && a == c, "grouped and split bursts match");
        check(a.size() == 4 && a[0].sample == 21 && a[1].sample == 36, "origin 5 opens at samples 21 and 36");
    }

    // After the line goes idle, the next origin starts a new spacing.
    {
        Uart uart;
        const uint8_t first[] = {7};
        check(uart.enqueue(0, first), "idle setup");
        Uart::Arrival peeked;
        check(!uart.peek(15, peeked), "sample 15 is before the ceiling");
        check(uart.peek(16, peeked) && peeked.value == 7 && peeked.sample == 16, "peek does not consume");
        check(uart.peek(16, peeked) && uart.size() == 1, "a second peek still sees the byte");
        check(!uart.take(15, peeked) && uart.size() == 1, "take before arrival leaves the queue");
        check(uart.take(16, peeked), "take at the arrival sample");
        const uint8_t again[] = {8};
        check(uart.enqueue(100, again), "reuse after the line is idle");
        const auto got = drain(uart, 116);
        check(got.size() == 1 && got[0].value == 8 && got[0].sample == 116, "idle origin 100 arrives at 116");
    }

    // A backlog is taken across consumer blocks without reordering.
    {
        Uart uart;
        const auto packet = bytes(6, 40);
        check(uart.enqueue(0, packet), "backlog enqueue");
        const auto first = drain(uart, 40);   // 16 and 31; 46 is still ahead
        const auto second = drain(uart, 70);  // 46 and 62; 77 is still ahead
        const auto third = drain(uart, 100);  // 77 and 92
        check(first.size() == 2 && second.size() == 2 && third.size() == 2, "three blocks share the backlog");
        const uint64_t when[] = {16, 31, 46, 62, 77, 92};
        const std::vector<Item> blocks[] = {first, second, third};
        bool ok = uart.empty() && first.size() == 2 && second.size() == 2 && third.size() == 2;
        for (size_t block = 0; ok && block < 3; ++block) {
            for (size_t i = 0; i < 2; ++i) {
                const size_t n = block * 2 + i;
                ok &= blocks[block][i].value == packet[n] && blocks[block][i].sample == when[n];
            }
        }
        check(ok, "blocks keep the line order and the known samples");
    }

    // Overflow keeps the previous bytes. A rejected origin does not stick.
    {
        Uart uart;
        const auto full = bytes(Uart::kCapacity, 0);
        check(uart.enqueue(0, full), "capacity fills exactly");
        check(uart.size() == Uart::kCapacity, "size is the capacity");
        const uint8_t extra[] = {1, 2};
        check(!uart.enqueue(80, extra), "one past capacity is rejected");
        check(uart.size() == Uart::kCapacity, "overflow changes nothing");
        check(uart.enqueue(0, std::span<const uint8_t>{}), "the rejected origin was not recorded");
        Uart::Arrival arrival;
        check(uart.take(100000, arrival) && arrival.value == 0, "the first queued byte is still first");
        const uint8_t single[] = {9};
        check(uart.enqueue(0, single), "a freed slot accepts one byte");
        check(uart.size() == Uart::kCapacity, "the queue is full again");
        check(uart.take(100000, arrival), "make a single free slot");
        check(uart.size() == Uart::kCapacity - 1, "one slot is free");
        check(!uart.enqueue(0, extra), "a span larger than the free space is refused whole");
        check(uart.size() == Uart::kCapacity - 1, "the refused span added nothing");
    }

    // Drain the ring, then fill it again so the indices wrap.
    {
        Uart uart;
        const auto first = bytes(200, 1);
        const auto second = bytes(200, 90);
        check(uart.enqueue(0, first), "wrap setup");
        check(drain(uart, 100000).size() == 200, "wrap setup drained");
        check(uart.enqueue(100000, second), "reuse after idle wraps the ring");
        const auto got = drain(uart, UINT64_MAX);
        bool same = got.size() == second.size();
        for (size_t i = 0; same && i < got.size(); ++i) same &= got[i].value == second[i];
        check(same, "wrapped slots return the new bytes");
    }

    // clear keeps the wire time. reset releases it.
    {
        Uart uart;
        const uint8_t two[] = {1, 2};
        const uint8_t one[] = {3};
        check(uart.enqueue(0, two), "clear setup");
        uart.clear();
        check(uart.empty(), "clear drops the queued bytes");
        check(uart.enqueue(0, one), "enqueue after clear");
        auto got = drain(uart, 100);
        check(got.size() == 1 && got[0].sample == 46, "clear leaves the line busy, so the next byte is the third slot");
        uart.reset();
        check(uart.enqueue(0, one), "enqueue after reset");
        got = drain(uart, 100);
        check(got.size() == 1 && got[0].value == 3 && got[0].sample == 16, "reset starts the spacing over");
        check(uart.enqueue(20, std::span<const uint8_t>{}), "an empty span records the origin");
        check(!uart.enqueue(19, one), "a backward origin is rejected");
        check(uart.empty(), "the rejected byte was not stored");
    }

    // Line time that cannot be represented rejects the whole span.
    {
        constexpr uint64_t kMaxOrigin = (UINT64_MAX - Uart::kSpacingNumer - (Uart::kSpacingDenom - 1)) / Uart::kSpacingDenom;
        Uart uart;
        const uint8_t one[] = {9};
        const uint8_t two[] = {4, 5};
        check(uart.enqueue(kMaxOrigin, one), "the last representable origin accepts one byte");
        Uart::Arrival arrival;
        check(uart.peek(UINT64_MAX, arrival) && arrival.value == 9, "that byte is queued");
        uart.reset();
        check(uart.enqueue(0, one), "arithmetic setup");
        check(!uart.enqueue(kMaxOrigin, two), "two bytes at the limit do not fit in the line clock");
        check(uart.enqueue(0, std::span<const uint8_t>(two, 1)), "the failed span did not move the line");
        const auto got = drain(uart, 100);
        check(got.size() == 2 && got[0].value == 9 && got[0].sample == 16 && got[1].value == 4 && got[1].sample == 31,
              "line time is unchanged by the rejected span");
        check(!uart.enqueue(kMaxOrigin + 1, one), "past the last origin is rejected");
    }

    if (failures) return 1;
    std::printf("test_native_sound_uart: ok\n");
    return 0;
}
