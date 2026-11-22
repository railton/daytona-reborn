#include "runtime/mobile_identity.h"

#include <cstdio>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

int main() {
    rt::MobileIdentity keys;
    constexpr uint32_t opcode = 1u << 23;
    constexpr uint32_t slot = 0x506680;
    const rt::MobileKey first{0x93dc, slot, 0};
    keys.write_header(23, opcode, 0x17c10, 0x93dc, slot, 10);
    CHECK(keys.read(23, opcode, 9) == rt::MobileKey{});
    CHECK(keys.read(23, opcode, 11) == first);
    CHECK(keys.read(23, opcode + 1, 11) == rt::MobileKey{});
    keys.clear_word(23);
    CHECK(keys.read(23, opcode, 11) == rt::MobileKey{});
    keys.write_header(23, opcode, 0x17c10, 0x93dc, slot, 11);
    keys.reset_slot(slot, 12);
    keys.reset_slot(slot, 12); // repeated writes during the guest's reset routine
    CHECK(keys.read(23, opcode, 12) == rt::MobileKey{});
    keys.write_header(23, opcode, 0x17c10, 0x93dc, slot, 12);
    CHECK(keys.read(23, opcode, 13) == rt::MobileKey{});
    keys.write_header(23, opcode, 0x17c10, 0x93dc, slot, 13);
    const rt::MobileKey next{0x93dc, slot, 1};
    CHECK(keys.read(23, opcode, 14) == next);
    keys.write_header(23, opcode, 0x17d78, 0x93dc, slot, 14);
    CHECK(keys.read(23, opcode, 14) == rt::MobileKey{});
    const rt::MobileKey body{0x7d68, 0, 0};
    keys.write_header(23, opcode, 0x17c10, 0x7d7c, 0, 15);
    CHECK(keys.read(23, opcode, 15) == body);
    keys.write_header(24, opcode, 0x17c10, 0x7d68, 0, 15);
    CHECK(keys.read(24, opcode, 15) == body);

    // Wheel identity is gated independently from the established vehicle keys.
    keys.write_header(25, opcode, 0x17d78, rt::opponent_wheel_near, slot, 15);
    CHECK(keys.read(25, opcode, 15) == rt::MobileKey{});
    rt::MobileIdentity wheels(true);
    const rt::MobileKey near_wheel{rt::opponent_wheel_near, slot, 0};
    const rt::MobileKey mid_wheel{rt::opponent_wheel_mid, slot, 1};
    wheels.write_header(25, opcode, 0x17d78, near_wheel.part, slot, 15);
    CHECK(wheels.read(25, opcode, 15) == near_wheel);
    wheels.reset_slot(slot, 16);
    CHECK(wheels.read(25, opcode, 16) == rt::MobileKey{});
    wheels.write_header(25, opcode, 0x17d78, mid_wheel.part, slot, 16);
    CHECK(wheels.read(25, opcode, 17) == rt::MobileKey{});
    wheels.write_header(25, opcode, 0x17d78, mid_wheel.part, slot, 17);
    CHECK(wheels.read(25, opcode, 17) == mid_wheel);
    wheels.write_header(25, opcode, 0x17c10, mid_wheel.part, slot, 18);
    CHECK(wheels.read(25, opcode, 18) == rt::MobileKey{});
    CHECK(rt::opponent_wheel_chassis_part(near_wheel.part) == 0x8808u);
    CHECK(rt::opponent_wheel_chassis_part(mid_wheel.part) == 0x8c00u);
}
