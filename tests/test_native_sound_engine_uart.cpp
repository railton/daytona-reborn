// Immediate mode stays on the three-argument engine. UART mode is opt-in.
// Synthetic program bytes only: no ROM.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime/native_sound_engine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void put16(std::vector<uint8_t> &p, unsigned at, unsigned value) {
    p.at(at) = uint8_t(value >> 8);
    p.at(at + 1) = uint8_t(value);
}
void put32(std::vector<uint8_t> &p, unsigned at, unsigned value) {
    put16(p, at, value >> 16);
    put16(p, at + 2, value);
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> p(0x40000);
    put32(p, 0x505a, 0x10000);
    put32(p, 0x505e, 0x11000);
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned at = i < 10 ? 0x10000 + 10 * i : 0x11000 + 10 * (i - 10);
        p[at] = 0x80;
        p[at + 1] = uint8_t(i);
        p[at + 2] = 255;
        p[at + 7] = uint8_t(i >= 10);
    }
    for (unsigned i = 0; i < 16; ++i) put32(p, 0x523a + i * 4, 0x14000);
    put32(p, 0x56ea, 0x15000);
    for (unsigned i = 0; i < 128; ++i) {
        p[0x5c1a + i] = 0x50;
        p[0x5cce + i] = uint8_t(i * 2 + 1);
        put32(p, 0x5dce + i * 4, 0x16000);
    }
    p[0x16001] = 255;
    p[0x16002] = 5;
    put32(p, 0x2bc6 + 4 * 0x21, 0x18000);
    put16(p, 0x18000, 0);
    put32(p, 0x18002, 0x19000);
    p[0x19000] = 0x80;
    const uint8_t song[] = {0x9a, 60, 127, 1, 0x9a, 61, 127, 1, 0xff, 0x2f};
    std::copy(std::begin(song), std::end(song), p.begin() + 0x19001);
    return p;
}
std::vector<uint8_t> sample() {
    std::vector<uint8_t> pcm(0x2000);
    auto *h = pcm.data() + 5 * 12;
    h[1] = 0x18;
    h[5] = 0xff;
    h[6] = 0x00;
    std::fill(pcm.begin() + 0x1800, pcm.begin() + 0x1900, 80);
    return pcm;
}

uint64_t hash_samples(const float *data, size_t count) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < count; ++i)
        hash = (hash ^ std::bit_cast<uint32_t>(data[i])) * 1099511628211ull;
    return hash;
}

bool audible(snd::NativeSoundEngine &engine, size_t frames) {
    std::array<float, 512> out{};
    engine.render(out.data(), frames);
    return std::any_of(out.begin(), out.begin() + frames * 2, [](float x) { return x != 0; });
}
}  // namespace

int main() {
    const uint8_t note[] = {0x90, 60, 127};
    const uint8_t off[] = {0x80, 60, 0};

    std::array<float, 1024> implicit_out{};
    std::array<float, 1024> explicit_out{};
    {
        snd::NativeSoundEngine engine(fixture(), sample(), sample());
        engine.send(note, sizeof note);
        engine.render(implicit_out.data(), 256);
        engine.send(off, sizeof off);
        engine.render(implicit_out.data() + 512, 256);
        check(engine.stats().notes == 1 && engine.stats().bytes == 6, "immediate note and note-off");
        check(std::any_of(implicit_out.begin(), implicit_out.end(), [](float x) { return x != 0; }),
              "immediate output is audible");
    }
    {
        snd::NativeSoundEngine engine(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::Immediate);
        engine.send(note, sizeof note);
        engine.render(explicit_out.data(), 256);
        engine.send(off, sizeof off);
        engine.render(explicit_out.data() + 512, 256);
        check(!engine.enqueue_at(0, note), "immediate mode refuses UART enqueue");
        check(engine.stats().bytes == 6, "refused enqueue does not add bytes");
    }
    const uint64_t implicit_hash = hash_samples(implicit_out.data(), implicit_out.size());
    const uint64_t explicit_hash = hash_samples(explicit_out.data(), explicit_out.size());
    check(implicit_hash == explicit_hash, "default and explicit immediate modes match");
    std::printf("immediate samples hash %016llx\n", (unsigned long long)implicit_hash);

    snd::NativeSoundEngine engine(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::SerializedUart);
    bool send_refused = false;
    try {
        engine.send(note, sizeof note);
    } catch (const std::logic_error &) {
        send_refused = true;
    }
    check(send_refused && engine.stats().bytes == 0, "UART mode refuses immediate send");
    check(engine.enqueue_at(0, note), "enqueue the note at the origin");
    check(!audible(engine, 45), "the line is still silent before the last byte");
    check(engine.stats().bytes == 2 && engine.stats().notes == 0, "two bytes have arrived by frame 45");
    check(!audible(engine, 1), "frame 46 accepts the note before its attack");
    check(engine.stats().frames == 46 && engine.stats().bytes == 3 && engine.stats().notes == 1,
          "the third byte lands on frame 46");
    const uint8_t reset = 0xff;
    check(!engine.enqueue_at(45, {&reset, 1}), "an origin behind the cursor is refused");
    check(engine.stats().bytes == 3 && engine.stats().voices == 1, "the refused origin leaves the engine");
    check(engine.wire_space() == snd::NativeSoundUart::kCapacity, "the refused byte is not queued");
    check(audible(engine, 1), "the accepted note becomes audible");

    for (uint64_t origin : {94ull, 95ull}) {
        snd::NativeSoundEngine tie(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::SerializedUart);
        const uint8_t track[] = {0xaa, 0x21, 0};
        check(tie.enqueue_at(0, track), "track command");
        check(tie.enqueue_at(origin, {&reset, 1}), "reset stays ordered after the track");
        check(audible(tie, 111), "the tie renders");
        check(tie.stats().bytes == 4 && tie.stats().voices == 0, "track and reset are consumed");
        check(tie.stats().notes == (origin == 95 ? 2 : 1), "sample 95 keeps both notes");
    }

    snd::NativeSoundEngine block(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::SerializedUart);
    check(block.enqueue_at(210, note), "note inside the first block");
    check(block.enqueue_at(480, off), "note-off in a later block");
    check(!audible(block, 256), "first block holds the note");
    check(block.stats().notes == 1 && block.stats().bytes == 3, "the note is complete inside 256 frames");
    check(audible(block, 256), "second block is audible");
    check(block.stats().bytes == 5 && block.sequence_stats().note_offs == 0,
          "the note-off is still split at frame 512");
    check(audible(block, 13), "frames through 525");
    check(block.stats().frames == 525 && block.sequence_stats().note_offs == 0,
          "frame 525 is the last frame before the note-off");
    check(audible(block, 1), "frame 526");
    check(block.stats().frames == 526 && block.stats().bytes == 6 && block.sequence_stats().note_offs == 1,
          "the split note-off lands on frame 526");
    check(audible(block, 256), "tail stays audible");
    check(block.sequence_stats().note_offs == 1, "the note-off is not repeated");

    // A data byte with no preceding status is an invalid sequence. The first
    // render faults the UART engine; the next render and enqueue do nothing.
    {
        snd::NativeSoundEngine bad(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::SerializedUart);
        const uint8_t data = 0x40;
        check(bad.enqueue_at(0, {&data, 1}), "queue a data byte without status");
        const uint64_t before = bad.stats().frames;
        bool threw = false;
        try {
            std::array<float, 64> out{};
            bad.render(out.data(), 16);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw, "the invalid byte faults the render");
        const uint64_t frozen = bad.stats().frames;
        bool threw_again = false;
        try {
            std::array<float, 64> out{};
            bad.render(out.data(), 16);
        } catch (const std::runtime_error &) {
            threw_again = true;
        }
        check(threw_again && bad.stats().frames == frozen, "the second render fails without advancing");
        check(!bad.enqueue_at(frozen, note), "enqueue refuses the faulted engine");
        check(bad.stats().frames == frozen, "the refused enqueue leaves the cursor");
        check(frozen >= before, "the fault does not rewind the cursor");

        snd::NativeSoundEngine fresh(fixture(), sample(), sample(), snd::NativeSoundEngine::Timing::SerializedUart);
        check(fresh.enqueue_at(0, note), "a replacement engine accepts a note");
        check(!audible(fresh, 45), "the replacement stays silent before the note");
        check(!audible(fresh, 1), "the replacement accepts the note on frame 46");
        check(fresh.stats().frames == 46 && fresh.stats().bytes == 3 && fresh.stats().notes == 1,
              "a replacement engine plays the same note");
    }

    if (failures) return 1;
    std::puts("native sound engine uart: immediate match, block edge, split note-off, late origin, fault");
    return 0;
}
