// app::SelectionSteer (src/app/selection_steer.h): keys and sticks pick the
// course and gearbox one push at a time, the choice held when released; a
// wheel passes through; each screen starts on its first option.

#include "app/selection_steer.h"

#include <cstdio>
#include <map>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct FakeBoard {
    std::map<uint32_t, uint8_t> ram;
    uint8_t read_byte(uint32_t a) const {
        const auto it = ram.find(a);
        return it == ram.end() ? 0 : it->second;
    }
};

// Holds steer for frames frames, then returns what the cabinet got last.
uint8_t hold(app::SelectionSteer &s, FakeBoard &b, uint8_t steer, int frames, bool wheel = false) {
    uint8_t out = steer;
    for (int i = 0; i < frames; ++i) {
        out = steer;
        s.apply(b, out, wheel);
    }
    return out;
}
} // namespace

int main() {
    using S = app::SelectionSteer;
    FakeBoard b;
    S s;
    // attract: untouched
    b.ram[S::mode_address] = 3;
    check(hold(s, b, 0xe0, 3) == 0xe0, "outside the selection, steering passes through");

    // the course screen
    b.ram[S::mode_address] = S::selection;
    b.ram[S::step_address] = S::course_step;
    check(hold(s, b, 0x80, 3) == 0x80, "course: starts on Beginner");
    check(hold(s, b, 0xe0, 5) == 0xa6, "course: one push right is Advanced");
    check(hold(s, b, 0x80, 5) == 0xa6, "course: released, Advanced stays");
    hold(s, b, 0xe0, 5);
    check(hold(s, b, 0x80, 5) == 0xe0, "course: a second push is Expert");
    hold(s, b, 0xe0, 5);
    check(hold(s, b, 0x80, 5) == 0xe0, "course: Expert is the last");
    hold(s, b, 0x20, 5);
    check(hold(s, b, 0x80, 5) == 0xa6, "course: a push left goes back to Advanced");
    check(hold(s, b, 0xa0, 5) == 0xa6, "course: a stick between centre and half lock does not move it");

    // the gearbox screen: a new screen, first option
    b.ram[S::step_address] = S::gearbox_step;
    check(hold(s, b, 0x80, 2) == 0x80, "gearbox: starts on the first option");
    hold(s, b, 0xe0, 5);
    check(hold(s, b, 0x80, 5) == 0xd0, "gearbox: one push right is the second, held");
    check(hold(s, b, 0x40, 5, true) == 0x40, "gearbox: a wheel passes through");

    // back on the course screen: Beginner again
    b.ram[S::step_address] = S::course_step;
    check(hold(s, b, 0x80, 2) == 0x80, "course again: starts on Beginner");

    std::puts(failures ? "selection_steer FAILED" : "selection_steer ok");
    return failures ? 1 : 0;
}
