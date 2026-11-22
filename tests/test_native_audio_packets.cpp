// SPSC timestamped audio packets. No SDL, no engine, no ROM.
#define NATIVE_AUDIO_PACKET_TESTING
#include "app/native_audio_packets.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using Queue = app::NativeAudioPacketQueue<4, 8>;

void pattern(uint8_t *dest, size_t n, uint64_t origin) {
    for (size_t i = 0; i < n; ++i) dest[i] = static_cast<uint8_t>(origin * 31u + i * 17u);
}

bool matches(std::span<const uint8_t> got, uint64_t origin) {
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != static_cast<uint8_t>(origin * 31u + i * 17u)) return false;
    return true;
}
}  // namespace

int main() {
    // Distinct packets, peeked before they are removed.
    {
        Queue queue;
        const uint8_t a[] = {1, 2, 3};
        const uint8_t b[] = {9, 8, 7, 6};
        const uint8_t c[] = {4};
        check(queue.try_publish(10, a) && queue.try_publish(20, b) && queue.try_publish(30, c), "burst publishes");
        check(queue.size() == 3, "burst size");
        Queue::Header header;
        uint8_t dest[8] = {};
        check(queue.peek(header) && header.origin == 10 && header.size == 3, "peek leaves the first packet");
        check(queue.size() == 3, "peek does not consume");
        check(queue.try_accept(dest, header) && header.origin == 10 && header.size == 3 &&
                  std::memcmp(dest, a, 3) == 0,
              "accept copies the first packet");
        check(queue.try_accept(dest, header) && header.origin == 20 && header.size == 4 &&
                  std::memcmp(dest, b, 4) == 0,
              "second packet");
        check(queue.try_accept(dest, header) && header.origin == 30 && header.size == 1 && dest[0] == 4,
              "third packet");
        check(queue.size() == 0 && !queue.peek(header), "burst drained");
        const uint8_t none[] = {0};
        check(queue.try_publish(11, std::span<const uint8_t>(none, 0)), "empty payload is a whole packet");
        check(queue.peek(header) && header.origin == 11 && header.size == 0, "empty payload keeps its origin");
        check(queue.try_accept({}, header) && header.size == 0, "empty payload accepts into an empty buffer");
    }

    // Fill, refuse the next, consume one, publish into the wrapped slot.
    {
        Queue queue;
        uint8_t body[4];
        for (uint64_t origin = 1; origin <= 4; ++origin) {
            pattern(body, 4, origin);
            check(queue.try_publish(origin, std::span<const uint8_t>(body, 4)), "fill a slot");
        }
        pattern(body, 4, 99);
        check(!queue.try_publish(99, std::span<const uint8_t>(body, 4)), "a full queue rejects");
        check(queue.size() == 4, "rejection does not drop a packet");
        const uint8_t too_big[9] = {};
        check(!queue.try_publish(100, too_big), "an oversized payload is rejected");
        check(queue.size() == 4, "the oversized payload is not queued");
        Queue::Header header;
        uint8_t dest[8] = {};
        uint8_t short_dest[3] = {};
        check(!queue.try_accept(short_dest, header), "a short buffer does not accept");
        check(queue.peek(header) && header.origin == 1 && header.size == 4 && queue.size() == 4,
              "the refused accept leaves the head");
        bool intact = true;
        for (uint64_t origin = 1; origin <= 4; ++origin) {
            intact &= queue.try_accept(dest, header) && header.origin == origin && header.size == 4 &&
                      matches(std::span<const uint8_t>(dest, 4), origin);
        }
        check(intact && queue.size() == 0, "overflow left the four packets intact");
    }

    // Wrap: two live packets, consume one, reuse the freed slot, then the rest.
    {
        Queue queue;
        uint8_t body[2];
        for (uint64_t origin = 1; origin <= 4; ++origin) {
            pattern(body, 2, origin);
            check(queue.try_publish(origin, std::span<const uint8_t>(body, 2)), "wrap fill");
        }
        Queue::Header header;
        uint8_t dest[8] = {};
        check(queue.try_accept(dest, header) && header.origin == 1 && matches(std::span<const uint8_t>(dest, 2), 1),
              "free one slot");
        pattern(body, 2, 5);
        check(queue.try_publish(5, std::span<const uint8_t>(body, 2)), "publish into the wrapped slot");
        bool wrapped = true;
        const uint64_t expect[] = {2, 3, 4, 5};
        for (uint64_t origin : expect) {
            wrapped &= queue.try_accept(dest, header) && header.origin == origin && header.size == 2 &&
                       matches(std::span<const uint8_t>(dest, 2), origin);
        }
        check(wrapped && queue.size() == 0 && !queue.peek(header), "wrapped order");
    }

    // reset is single-threaded and drops whatever was queued.
    {
        Queue queue;
        const uint8_t body[] = {1, 2, 3, 4};
        check(queue.try_publish(7, body), "reset setup");
        queue.reset();
        Queue::Header header;
        check(queue.size() == 0 && !queue.peek(header), "reset is empty");
        const uint8_t next[] = {8, 8};
        check(queue.try_publish(8, next), "publish after reset");
        uint8_t dest[8] = {};
        check(queue.try_accept(dest, header) && header.origin == 8 && header.size == 2 && dest[0] == 8 && dest[1] == 8,
              "reset forgot the previous packet");
        check(queue.size() == 0, "nothing follows the reset packet");
        check(queue.stage_empty(1) && queue.size() == 0, "staging an empty queue leaves it empty");
        queue.reset();
    }

    // uint32 max and 0 are both 0 mod 3. Indexing the slot by the counter
    // would publish the second packet onto the first. The slot cursor is
    // separate, so three packets keep their bytes across that boundary.
    // One publish and accept first leaves the cursor off zero as well.
    {
        static_assert(0u % 3u == 0u);
        static_assert(0xffffffffu % 3u == 0u);
        using Pipe = app::NativeAudioPacketQueue<3, 8>;
        Pipe queue;
        const uint8_t prime[] = {7, 7};
        check(queue.try_publish(1, prime), "prime the slot cursor");
        Pipe::Header header;
        uint8_t dest[8] = {};
        check(queue.try_accept(dest, header) && header.origin == 1, "cursor leaves slot zero");
        check(queue.stage_empty(0xffffffffu), "stage the empty queue at the counter maximum");
        const uint8_t occupied[] = {1};
        check(queue.try_publish(50, occupied), "occupy one slot");
        check(!queue.stage_empty(1), "stage refuses a queue that is not empty");
        check(queue.try_accept(dest, header) && header.origin == 50 && dest[0] == 1,
              "the refused stage left the packet");
        check(queue.stage_empty(0xffffffffu), "stage again at the counter maximum");
        bool published = true;
        for (uint64_t origin = 10; origin < 13; ++origin) {
            uint8_t body[4];
            pattern(body, 4, origin);
            published &= queue.try_publish(origin, body);
        }
        uint8_t extra[4];
        pattern(extra, 4, 99);
        check(published && !queue.try_publish(99, extra) && queue.size() == 3,
              "three packets fill the queue across the counter wrap");
        bool intact = true;
        for (uint64_t origin = 10; origin < 13; ++origin) {
            intact &= queue.try_accept(dest, header) && header.origin == origin && header.size == 4 &&
                      matches(std::span<const uint8_t>(dest, 4), origin);
        }
        check(intact && queue.size() == 0, "wrap did not overwrite a live packet");
    }

    // The slot is released only after the callable accepts. false and an
    // exception leave the same head, so the queue stays full. The following
    // accept takes that packet, then the next origin, and one new publish fits.
    {
        Queue queue;
        uint8_t body[4];
        for (uint64_t origin = 1; origin <= 4; ++origin) {
            pattern(body, 4, origin);
            check(queue.try_publish(origin, std::span<const uint8_t>(body, 4)), "conditional fill");
        }
        auto head_is = [&](uint64_t origin) {
            Queue::Header header;
            return queue.peek(header) && header.origin == origin && header.size == 4 && queue.size() == 4;
        };
        const uint8_t *slot = nullptr;
        int seen = 0;
        check(!queue.try_consume([&](Queue::Header head, std::span<const uint8_t> payload) {
                  ++seen;
                  slot = payload.data();
                  check(head.origin == 1 && head.size == 4 && matches(payload, 1), "false sees the head");
                  return false;
              }),
              "false does not consume");
        check(seen == 1 && slot != nullptr && head_is(1), "false leaves the same head");
        pattern(body, 4, 99);
        check(!queue.try_publish(99, std::span<const uint8_t>(body, 4)) && queue.size() == 4,
              "false did not free a slot");

        seen = 0;
        bool threw = false;
        try {
            queue.try_consume([&](Queue::Header head, std::span<const uint8_t> payload) -> bool {
                ++seen;
                check(payload.data() == slot && head.origin == 1 && head.size == 4 && matches(payload, 1),
                      "throw sees the same slot");
                throw std::runtime_error("refuse");
            });
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw && seen == 1 && head_is(1), "throw leaves the same head and the counters");
        check(!queue.try_publish(99, std::span<const uint8_t>(body, 4)) && queue.size() == 4,
              "throw did not free a slot");

        seen = 0;
        check(queue.try_consume([&](Queue::Header head, std::span<const uint8_t> payload) {
                  ++seen;
                  return head.origin == 1 && head.size == 4 && payload.data() == slot && matches(payload, 1);
              }),
              "true accepts the same origin and payload");
        check(seen == 1 && queue.size() == 3, "acceptance frees one slot");
        Queue::Header header;
        check(queue.peek(header) && header.origin == 2 && header.size == 4, "the next packet follows");
        pattern(body, 4, 5);
        check(queue.try_publish(5, std::span<const uint8_t>(body, 4)) && queue.size() == 4,
              "capacity allows one publish after the release");
        bool order = true;
        uint8_t dest[8] = {};
        const uint64_t expect[] = {2, 3, 4, 5};
        for (uint64_t origin : expect) {
            order &= queue.try_accept(dest, header) && header.origin == origin && header.size == 4 &&
                     matches(std::span<const uint8_t>(dest, 4), origin);
        }
        check(order && queue.size() == 0, "conditional release kept the following order");
        seen = 0;
        check(!queue.try_consume([&](Queue::Header, std::span<const uint8_t>) {
                  ++seen;
                  return true;
              }),
              "an empty queue does not call accept");
        check(seen == 0 && queue.size() == 0, "empty consume leaves the counters");
    }

    // Concurrent producer and consumer. Small capacity, many distinct packets.
    {
        constexpr int kCount = 4000;
        using Pipe = app::NativeAudioPacketQueue<3, 16>;
        Pipe queue;
        std::atomic<int> problems{0};
        std::atomic<bool> stop{false};
        std::thread producer([&] {
            uint8_t body[16];
            for (int i = 0; i < kCount && !stop.load(std::memory_order_relaxed); ++i) {
                pattern(body, 16, static_cast<uint64_t>(i));
                while (!queue.try_publish(static_cast<uint64_t>(i), body)) {
                    if (stop.load(std::memory_order_relaxed)) return;
                    std::this_thread::yield();
                }
            }
        });
        std::thread consumer([&] {
            uint8_t dest[16];
            for (int i = 0; i < kCount; ++i) {
                Pipe::Header header;
                for (;;) {
                    Pipe::Header seen;
                    if (!queue.peek(seen)) {
                        std::this_thread::yield();
                        continue;
                    }
                    if (seen.origin != static_cast<uint64_t>(i) || seen.size != 16) {
                        problems.fetch_add(1, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (!queue.try_accept(dest, header)) {
                        problems.fetch_add(1, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (header.origin != static_cast<uint64_t>(i) || header.size != 16 ||
                        !matches(std::span<const uint8_t>(dest, 16), static_cast<uint64_t>(i))) {
                        problems.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        });
        producer.join();
        consumer.join();
        check(problems.load() == 0, "concurrent origins and payloads");
        check(queue.size() == 0, "size is exact once both roles have stopped");
    }

    // An observer samples size() while both roles run. Every sample stays
    // inside the capacity; once they have stopped, size() is the occupancy.
    {
        using Pipe = app::NativeAudioPacketQueue<3, 8>;
        Pipe queue;
        std::atomic<bool> run{true};
        std::thread producer([&] {
            uint8_t body[4];
            uint64_t origin = 0;
            while (run.load(std::memory_order_relaxed)) {
                pattern(body, 4, origin);
                if (queue.try_publish(origin, std::span<const uint8_t>(body, 4))) ++origin;
                else std::this_thread::yield();
            }
        });
        std::thread consumer([&] {
            uint8_t dest[8];
            while (run.load(std::memory_order_relaxed)) {
                Pipe::Header header;
                if (!queue.try_accept(dest, header)) std::this_thread::yield();
            }
        });
        int out_of_range = 0;
        constexpr int kSamples = 100000;
        for (int i = 0; i < kSamples; ++i) {
            if (queue.size() > Pipe::kCapacity) ++out_of_range;
        }
        run.store(false, std::memory_order_relaxed);
        producer.join();
        consumer.join();
        Pipe::Header header;
        uint8_t dest[8];
        while (queue.try_accept(dest, header)) {
        }
        check(out_of_range == 0, "concurrent size stays inside capacity");
        check(queue.size() == 0, "drained size is exact");
    }

    if (failures) return 1;
    std::printf("test_native_audio_packets: ok\n");
    return 0;
}
