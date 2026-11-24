// The communication board (src/runtime/comm_board.cpp) with two boards wired
// to each other in memory: a master and a slave, as the game sets them (fg 1
// on the master, 0 on the slave; cn starts the board). No ROM, no network.
// Checks the numbering token round the ring (cabinet 1 of 2, 2 of 2), that
// race data crosses each way into the frame offset, that frames split into
// small pieces still arrive whole, and that a closed line loses the link;
// then a ring of 3 numbered by FF/FE gives the ids rt::online::link_id
// computes, and the same ring preset (CommBoard::preset_ring) is up at once
// and passes data.

#include "runtime/comm_board.h"
#include "runtime/online_settings.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// One direction of cable.
struct Wire {
    std::deque<uint8_t> bytes;
    bool closed = false;
};

// A cabinet's two ends: reads from `in`, writes to `out`. `chunk` limits each
// read, as TCP may hand frames over in pieces.
class Loop : public rt::LinkTransport {
public:
    Loop(Wire &in, Wire &out, int chunk) : in_(in), out_(out), chunk_(chunk) {}
    void poll() override {}
    bool rx_open() const override { return !in_.closed || !in_.bytes.empty(); }
    bool tx_open() const override { return !out_.closed; }
    int read(uint8_t *buf, int max) override {
        if (in_.bytes.empty()) return in_.closed ? -1 : 0;
        const int n = std::min<int>({max, chunk_, int(in_.bytes.size())});
        for (int i = 0; i < n; i++) buf[i] = in_.bytes[size_t(i)];
        in_.bytes.erase(in_.bytes.begin(), in_.bytes.begin() + n);
        return n;
    }
    bool write(const uint8_t *buf, int n) override {
        if (out_.closed) return false;
        out_.bytes.insert(out_.bytes.end(), buf, buf + n);
        return true;
    }

private:
    Wire &in_, &out_;
    int chunk_;
};
} // namespace

int main() {
    Wire a_to_b, b_to_a;
    std::vector<uint8_t> ram_a(0x4000), ram_b(0x4000);
    Loop la(b_to_a, a_to_b, 1000), lb(a_to_b, b_to_a, 777);
    rt::CommBoard a(ram_a.data()), b(ram_b.data());
    a.set_transport(&la);
    b.set_transport(&lb);
    check(a.link() == rt::CommBoard::Link::Off, "off until the game starts the board");

    a.fg_w(1); // master
    b.fg_w(0); // slave
    a.cn_w(1);
    b.cn_w(1);
    check(a.link() == rt::CommBoard::Link::Waiting && ram_a[0x12] == 0x00 && ram_a[0x13] == 0x0e, "init: frame size");
    for (int frame = 0; frame < 400 && !(a.link() == rt::CommBoard::Link::Up && b.link() == rt::CommBoard::Link::Up); frame++) {
        a.vblank();
        b.vblank();
    }
    check(a.link() == rt::CommBoard::Link::Up && b.link() == rt::CommBoard::Link::Up, "link up");
    check(a.id() == 1 && a.count() == 2 && b.id() == 2 && b.count() == 2, "cabinets numbered 1 and 2 of 2");
    check(ram_a[0] == 1 && ram_a[2] == 1 && ram_a[3] == 2 && ram_b[0] == 1 && ram_b[2] == 2 && ram_b[3] == 2,
          "status in shared RAM");

    // Race data: each cabinet's block at 0x2000 lands in the other's at 0x21c0.
    for (int i = 0; i < 0x40; i++) ram_a[0x2000 + i] = uint8_t(0xa0 + i), ram_b[0x2000 + i] = uint8_t(0x50 + i);
    for (int frame = 0; frame < 4; frame++) {
        a.vblank();
        b.vblank();
        (void)a.fg_r(), (void)b.fg_r(); // the game polls fg: frames are taken in there too
    }
    bool a_got = true, b_got = true;
    for (int i = 0; i < 0x40; i++) {
        b_got &= ram_b[0x21c0 + i] == uint8_t(0xa0 + i);
        a_got &= ram_a[0x21c0 + i] == uint8_t(0x50 + i);
    }
    check(b_got, "master's data reaches the slave");
    check(a_got, "slave's data reaches the master");

    // The slave goes away: the master loses the link.
    b_to_a.closed = true;
    a_to_b.closed = true;
    for (int frame = 0; frame < 3; frame++) a.vblank();
    check(a.link() == rt::CommBoard::Link::Lost && ram_a[0] == 0xff, "a closed line loses the link");

    // A ring of 3 (place 1 the MASTER, then 2, then 3): the ids the numbering
    // hands out, then the same ring preset.
    for (const bool preset : {false, true}) {
        Wire w12, w23, w31;
        std::vector<uint8_t> r1(0x4000), r2(0x4000), r3(0x4000);
        Loop l1(w31, w12, 900), l2(w12, w23, 900), l3(w23, w31, 900);
        rt::CommBoard c1(r1.data()), c2(r2.data()), c3(r3.data());
        c1.set_transport(&l1), c2.set_transport(&l2), c3.set_transport(&l3);
        rt::CommBoard *ring[3] = {&c1, &c2, &c3};
        for (int place = 1; place <= 3; ++place) {
            ring[place - 1]->fg_w(place == 1 ? 1 : 0);
            if (preset) ring[place - 1]->preset_ring(rt::online::link_id(place, 3), 3);
        }
        for (auto *c : ring) c->cn_w(1);
        if (preset)
            check(c1.link() == rt::CommBoard::Link::Up && c2.link() == rt::CommBoard::Link::Up &&
                      c3.link() == rt::CommBoard::Link::Up,
                  "preset ring: up at once");
        int frames = 0;
        for (; frames < 400 && !(c1.link() == rt::CommBoard::Link::Up && c2.link() == rt::CommBoard::Link::Up &&
                                 c3.link() == rt::CommBoard::Link::Up);
             ++frames)
            for (auto *c : ring) c->vblank();
        check(c1.link() == rt::CommBoard::Link::Up && c2.link() == rt::CommBoard::Link::Up && c3.link() == rt::CommBoard::Link::Up,
              "ring of 3 up");
        for (int place = 1; place <= 3; ++place)
            check(ring[place - 1]->id() == rt::online::link_id(place, 3) && ring[place - 1]->count() == 3,
                  preset ? "preset ids" : "numbered ids match link_id");
        // the MASTER's data goes round
        for (int i = 0; i < 0x20; i++) r1[0x2000 + i] = uint8_t(0x30 + i);
        for (int frame = 0; frame < 6; frame++)
            for (auto *c : ring) c->vblank(), (void)c->fg_r();
        bool got = true;
        for (int i = 0; i < 0x20; i++) got &= r2[0x21c0 + i] == uint8_t(0x30 + i);
        check(got, preset ? "preset ring passes data" : "numbered ring passes data");
    }

    if (failures) return 1;
    std::printf("test_comm_board: ok\n");
    return 0;
}
