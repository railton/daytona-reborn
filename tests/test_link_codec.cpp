// app::online::LinkCodec (src/app/link_codec.h): wire frames shaped as the
// comm board's (an id and 8 slots of 448 bytes, 0xFC tokens, FF/FE
// handshakes) survive the round trip; blocks that change little become
// small; malformed messages are refused without reading past their end.
// Synthetic data only (the real sessions were checked against captures
// outside the repository: no mismatch in 2, 4 and 8 cabinet sessions).

#include "app/link_codec.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using Frame = std::vector<uint8_t>;
constexpr size_t kSlot = app::online::LinkCodec::kSlot, kBlock = 1 + 8 * kSlot;

bool round_trip(app::online::LinkCodec &enc, app::online::LinkCodec &dec, const Frame &f, size_t *size = nullptr) {
    std::string m, out;
    enc.encode(f.data(), f.size(), m);
    if (size) *size = m.size();
    if (!dec.decode(reinterpret_cast<const uint8_t *>(m.data()), m.size(), out)) return false;
    Frame want = f;
    if (f.size() >= 2 && f[0] == 0xfc) std::fill(want.begin() + 2, want.end(), 0); // a token keeps bytes 0 and 1
    return out == std::string(want.begin(), want.end());
}
} // namespace

int main() {
    std::mt19937 rng(7);
    app::online::LinkCodec enc, dec;

    // the link's handshake and odd frames go raw
    check(round_trip(enc, dec, Frame{0xff, 0x01, 0x00, 0x00}), "FF handshake");
    check(round_trip(enc, dec, Frame(kBlock, 0xfe)), "FE frame of a block's size");
    check(round_trip(enc, dec, Frame{0x03, 0x01, 0x02}), "a short frame");

    // a sender's blocks: a shift register of 8 slots, a few bytes of each changing a frame
    Frame block(kBlock, 0);
    block[0] = 2;
    size_t total = 0, frames = 0, largest = 0;
    for (int f = 0; f < 600; ++f) {
        for (int s = 7; s > 0; --s) // the slots move along, as the ring passes data on
            std::copy(block.begin() + 1 + (s - 1) * kSlot, block.begin() + 1 + s * kSlot, block.begin() + 1 + s * kSlot);
        for (int k = 0; k < 28; ++k) block[1 + rng() % 150] = uint8_t(rng()); // slot 0: this cabinet's ~28 changing bytes
        size_t n = 0;
        if (!round_trip(enc, dec, block, &n)) {
            check(false, "block round trip");
            break;
        }
        Frame token(kBlock, 0x55);
        token[0] = 0xfc;
        token[1] = uint8_t(f);
        size_t t = 0;
        if (!round_trip(enc, dec, token, &t)) {
            check(false, "token round trip");
            break;
        }
        check(t == 4, "a token is 4 bytes");
        if (f >= 2) total += n + t, ++frames, largest = std::max(largest, n);
    }
    std::printf("synthetic blocks: %.0f bytes a game frame (raw %zu), largest %zu\n", double(total) / frames, 2 * kBlock,
                largest);
    check(frames && total / frames < 400, "a block that changes little is small");

    // random noise (nothing to gain) still round-trips
    for (auto &b : block) b = uint8_t(rng());
    block[0] = 5;
    check(round_trip(enc, dec, block), "noise round trip");

    // a fresh codec on either side: the streams start again together (a new ring)
    enc.reset();
    dec.reset();
    check(round_trip(enc, dec, block), "after reset");

    // malformed messages: refused, nothing appended
    app::online::LinkCodec fresh;
    std::string out;
    std::string encoded;
    {
        app::online::LinkCodec e;
        e.encode(block.data(), block.size(), encoded);
        e.encode(block.data(), block.size(), encoded); // a second block referring to the first
    }
    const std::vector<std::string> bad = {
        std::string(),                     // empty
        std::string("\x07", 1),            // unknown kind
        std::string("\x01\x0e\x01", 3),    // token cut short
        std::string("\x01\x00\x00\x07", 4), // token of length 0
        std::string("\x02\x03", 2),        // a block with no slot
        std::string("\x02\x03\x11", 3),    // reference 17
        std::string("\x02\x03\x01\x00\x00", 5), // a reference to a block this side never had
        encoded.substr(0, encoded.size() / 3), // cut in the middle
    };
    for (const auto &m : bad) {
        const bool ok = fresh.decode(reinterpret_cast<const uint8_t *>(m.data()), m.size(), out);
        check(!ok && out.empty(), "malformed message refused");
    }
    // varint longer than the slot
    std::string huge("\x02\x03\x00\xff\xff\xff\x7f\x00", 8);
    check(!fresh.decode(reinterpret_cast<const uint8_t *>(huge.data()), huge.size(), out) && out.empty(),
          "a run past the slot refused");

    std::puts(failures ? "link_codec FAILED" : "link_codec ok");
    return failures ? 1 : 0;
}
