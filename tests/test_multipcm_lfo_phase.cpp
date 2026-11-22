// Synthetic MultiPCM LFO phase, with no game ROM and no scale tables.
// Two questions, answered by comparing chip output at one pitch:
// 1) Retrigger of the same slot keeps the LFO phase. A reset would make the
//    following audio match a voice that has just started.
// 2) Pitch depth 0 freezes that phase. Restoring the depth continues from
//    the phase held at the mute, which a still-running LFO would have left.
// The sample position is cleared on every compared tail, so only the phase
// history can move the waveform. Immediate retrigger, before any sample,
// matches a fresh voice: the retrigger itself adds nothing else.
#include "runtime/multipcm.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_wave(const std::vector<float> &got, const std::vector<float> &want, const char *what) {
    if (got == want) return;
    size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at]) ++at;
    std::fprintf(stderr, "FAIL: %s (first mismatch %zu)\n", what, at);
    ++failures;
}

// 256-byte 8-bit square at 0x1000, looped, instant attack, no header LFO.
std::vector<uint8_t> rom() {
    std::vector<uint8_t> r(0x2000, 0);
    r[0] = 0x00, r[1] = 0x10, r[2] = 0x00;
    r[3] = 0x00, r[4] = 0x00;
    r[5] = 0xff, r[6] = 0x00;
    r[8] = 0xf0;
    r[10] = 0x0f;
    for (int i = 0; i < 256; ++i) r[size_t(0x1000 + i)] = (i / 16) & 1 ? 0x40 : 0xc0;
    return r;
}

constexpr uint8_t kPitchOn = 0x3f;  // frequency 7, pitch depth 7
constexpr uint8_t kPitchOff = 0x38; // same frequency, pitch depth 0
constexpr int kEarly = 64;          // phase has moved; the scale is still flat
constexpr int kRun = 2048;          // long enough for the pitch scale to leave unity
constexpr int kGap = 512;
constexpr int kTail = 1024;

void reg(snd::MultiPcm &pcm, uint8_t address, uint8_t data) {
    pcm.write(2, address);
    pcm.write(0, data);
}

void key_on(snd::MultiPcm &pcm) {
    pcm.write(1, 0);
    reg(pcm, 0, 0x20);
    reg(pcm, 1, 0);
    reg(pcm, 2, 0);
    reg(pcm, 3, 0);
    reg(pcm, 5, 0x01);
    reg(pcm, 4, 0x80);
}

void pitch_depth(snd::MultiPcm &pcm, uint8_t pitch) {
    reg(pcm, 6, pitch);
    reg(pcm, 10, 0);
}

void retrigger(snd::MultiPcm &pcm) { reg(pcm, 4, 0x80); }

std::vector<float> take(snd::MultiPcm &pcm, int frames) {
    std::vector<float> left(static_cast<size_t>(frames));
    std::vector<float> right(static_cast<size_t>(frames));
    pcm.generate(left.data(), right.data(), frames);
    return left;
}

snd::MultiPcm chip(const std::vector<uint8_t> &wave) {
    return snd::MultiPcm(10000000, wave.data(), uint32_t(wave.size()));
}

// Play `run` frames with the pitch LFO on, retrigger, then the tail.
std::vector<float> retrigger_tail(const std::vector<uint8_t> &wave, int run, bool rekey = true) {
    snd::MultiPcm pcm = chip(wave);
    key_on(pcm);
    pitch_depth(pcm, kPitchOn);
    if (run) take(pcm, run);
    if (rekey) retrigger(pcm);
    return take(pcm, kTail);
}

// Same prefix, mute the pitch depth, restore it, then retrigger.
std::vector<float> frozen_tail(const std::vector<uint8_t> &wave) {
    snd::MultiPcm pcm = chip(wave);
    key_on(pcm);
    pitch_depth(pcm, kPitchOn);
    take(pcm, kRun);
    pitch_depth(pcm, kPitchOff);
    take(pcm, kGap);
    pitch_depth(pcm, kPitchOn);
    retrigger(pcm);
    return take(pcm, kTail);
}

// The gap stays modulated, so the phase keeps advancing, then retrigger.
std::vector<float> advanced_tail(const std::vector<uint8_t> &wave) {
    snd::MultiPcm pcm = chip(wave);
    key_on(pcm);
    pitch_depth(pcm, kPitchOn);
    take(pcm, kRun + kGap);
    retrigger(pcm);
    return take(pcm, kTail);
}

// The mute interval itself, and the same interval with the LFO left on.
std::vector<float> gap(const std::vector<uint8_t> &wave, bool mute) {
    snd::MultiPcm pcm = chip(wave);
    key_on(pcm);
    pitch_depth(pcm, kPitchOn);
    take(pcm, kRun);
    if (mute) pitch_depth(pcm, kPitchOff);
    return take(pcm, kGap);
}

bool audible(const std::vector<float> &wave) {
    for (float v : wave)
        if (v != 0.f) return true;
    return false;
}
} // namespace

int main() {
    const auto wave = rom();
    const auto fresh = retrigger_tail(wave, 0, false);
    const auto fresh_again = retrigger_tail(wave, 0, false);
    const auto early = retrigger_tail(wave, kEarly);
    const auto held = retrigger_tail(wave, kRun);
    const auto held_again = retrigger_tail(wave, kRun);
    const auto frozen = frozen_tail(wave);
    const auto frozen_again = frozen_tail(wave);
    const auto advanced = advanced_tail(wave);
    const auto muted = gap(wave, true);
    const auto running = gap(wave, false);

    check_wave(fresh, fresh_again, "a fresh tail is deterministic");
    check_wave(held, held_again, "a retriggered tail is deterministic");
    check_wave(frozen, frozen_again, "a restored tail is deterministic");
    check(audible(fresh) && audible(held) && audible(frozen) && audible(muted), "tails are not silent");

    // Phase still zero: retrigger matches a voice that has not run.
    check_wave(fresh, retrigger_tail(wave, 0), "retrigger before any sample matches a fresh voice");
    // kEarly is still inside the flat opening of the pitch scale. A phase
    // reset would reproduce the fresh tail. A kept phase reaches the first
    // scale change sooner, so the tails differ.
    check(early != fresh, "retrigger keeps a phase the scale has not audibly left yet");
    // After the scale has left unity, a reset would reproduce the opening.
    check(held != fresh, "retrigger after audible modulation does not restore the opening");
    // Depth 0 for kGap, then the same depth restored. The tail matches the
    // voice retriggered with no gap, and differs from a voice whose LFO ran
    // through that gap. The mute itself also differs from the running LFO,
    // so the freeze is not a mute that left the sound unchanged.
    check_wave(frozen, held, "pitch depth 0 freezes the phase");
    check(frozen != advanced, "a gap with the LFO left on moves the phase");
    check(advanced != held, "the continued LFO does not match the frozen phase");
    check(muted != running, "pitch depth 0 changes the sound during the gap");

    if (failures) return 1;
    std::printf("test_multipcm_lfo_phase: ok\n");
    return 0;
}
