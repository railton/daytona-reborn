// runtime/remote_cars.h: each other player's data step, by the car record
// that draws their car, a tick late. Synthetic RAM only (no ROM data).
#include "runtime/remote_cars.h"

#include <array>
#include <cstdio>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

namespace {
// Car records holding car-number bytes (+0x94; 8 a computer car) and the comm RAM.
struct FakeBoard {
    std::array<uint8_t, rt::MobileIdentity::slot_count> numbers{};
    std::array<uint8_t, 0x4000> comm{};
    FakeBoard() { numbers.fill(8); }
    uint8_t read_byte(uint32_t address) const {
        const uint32_t i = address - rt::online::kCarRecords;
        if (i % rt::online::kCarRecordSize != rt::online::kCarNumberOffset) return 0xee;
        return numbers.at(i / rt::online::kCarRecordSize);
    }
    const uint8_t *comm_ram() const { return comm.data(); }
    void tick(int k, uint8_t value) { comm[0x21c0 + k * 0x1c0 + 0x0a] = value; }
};
} // namespace

int main() {
    // A room of 4, this cabinet car 2 (ring place 2). Slot k holds the car
    // k + 1 places behind: k 0 car 1, k 1 car 4, k 2 car 3. Records: car 1 in
    // record 5, car 3 in record 0, car 4 in record 9; record 2 is this
    // cabinet's own number (never a remote car), the rest computer cars.
    FakeBoard b;
    b.numbers[5] = 0, b.numbers[0] = 2, b.numbers[9] = 3, b.numbers[2] = 1;
    rt::online::RemoteCarSteps steps;
    b.tick(0, 10), b.tick(1, 20), b.tick(2, 30);
    auto s = steps.update(b, 2, 4);
    for (int8_t v : s) CHECK(v == -1); // the first tick: nothing landed before it
    b.tick(0, 11), b.tick(1, 20), b.tick(2, 33);
    s = steps.update(b, 2, 4);
    CHECK(s[5] == 1 && s[9] == 1 && s[0] == 1); // the first landing counts 1
    CHECK(s[2] == -1 && s[1] == -1);
    b.tick(0, 12), b.tick(1, 22), b.tick(2, 33);
    s = steps.update(b, 2, 4);
    CHECK(s[5] == 1 && s[9] == 0 && s[0] == 3); // the previous tick's: car 1 +1, car 4 +0, car 3 +3
    s = steps.update(b, 2, 4);
    CHECK(s[5] == 1 && s[9] == 2 && s[0] == 0);
    b.tick(0, 4); // the counter wraps: 12 -> 4 is 248
    steps.update(b, 2, 4);
    s = steps.update(b, 2, 4);
    CHECK(s[5] == 100); // capped
    // out of a race: everything starts again
    steps.reset();
    s = steps.update(b, 2, 4);
    for (int8_t v : s) CHECK(v == -1);
    std::printf("remote cars: ok\n");
    return 0;
}
