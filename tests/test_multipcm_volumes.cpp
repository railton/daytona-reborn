// The MultiPCM's music and effects volumes (the launcher's Audio tab) on a
// synthetic sample: at 1 and 1 the output is the chip's own, bit for bit; a
// volume of 0 leaves exactly the other kind's voices. No game ROM data.
#include "runtime/multipcm.h"

#include <array>
#include <cmath>
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

// Sample 0: 256 bytes of 8-bit square wave at 0x1000, looped, instant attack.
std::vector<uint8_t> rom() {
    std::vector<uint8_t> r(0x2000, 0);
    r[0] = 0x00, r[1] = 0x10, r[2] = 0x00; // start 0x1000, 8-bit
    r[3] = 0x00, r[4] = 0x00;              // loop 0
    r[5] = 0xff, r[6] = 0x00;              // end 256
    r[8] = 0xf0;                           // attack 15, decay 1 0
    r[10] = 0x0f;                          // release 15
    // Sample 1: the same wave and envelope. Its header carries both LFO
    // registers: pitch frequency 7 and depth 7, tremolo depth 7.
    r[12] = 0x00, r[13] = 0x10, r[14] = 0x00;
    r[15] = 0x00, r[16] = 0x00;
    r[17] = 0xff, r[18] = 0x00;
    r[19] = 0x3f;
    r[20] = 0xf0;
    r[22] = 0x0f;
    r[23] = 0x07;
    for (int i = 0; i < 256; ++i) r[size_t(0x1000 + i)] = (i / 16) & 1 ? 0x40 : 0xc0;
    return r;
}

constexpr uint8_t kPitchLfo = 0x3f; // frequency 7, pitch depth 7
constexpr uint8_t kPitchOff = 0x38; // same frequency, pitch depth 0
constexpr uint8_t kTremolo = 0x07;

void key_on(snd::MultiPcm &pcm, uint8_t slot, uint8_t pan, uint8_t octave, bool effect, uint8_t sample = 0) {
    auto reg = [&](uint8_t r, uint8_t v) { pcm.write(2, r), pcm.write(0, v); };
    pcm.write(1, slot);
    reg(0, pan);
    reg(1, sample);
    reg(2, 0), reg(3, uint8_t(octave << 4)); // pitch
    reg(5, 0x01);                            // full level, no interpolation
    pcm.set_effect(effect);
    reg(4, 0x80);
}

void write_lfo(snd::MultiPcm &pcm, uint8_t pitch, uint8_t tremolo) {
    pcm.write(2, 6), pcm.write(0, pitch);
    pcm.write(2, 10), pcm.write(0, tremolo);
}

std::array<float, 512> render(snd::MultiPcm &pcm) {
    std::array<float, 512> out{};
    pcm.generate(out.data(), out.data() + 256, 256);
    return out;
}
} // namespace

int main() {
    const auto r = rom();
    auto chip = [&](bool music, bool effect, float mv, float ev, bool set) {
        snd::MultiPcm pcm(10000000, r.data(), uint32_t(r.size()));
        if (music) key_on(pcm, 0, 0x20, 0, false);
        if (effect) key_on(pcm, 1, 0xd0, 1, true);
        if (set) pcm.set_volumes(mv, ev);
        return render(pcm);
    };
    const auto both = chip(true, true, 1, 1, false), unity = chip(true, true, 1, 1, true);
    const auto music = chip(true, false, 1, 1, false), effects = chip(false, true, 1, 1, false);
    check(both == unity, "1 and 1: the chip's own output");
    check(chip(true, true, 1, 0, true) == music, "effects at 0: the music alone");
    check(chip(true, true, 0, 1, true) == effects, "music at 0: the effects alone");
    bool sounds = false;
    for (float v : effects) sounds |= v != 0;
    check(sounds, "the effect voice sounds");
    const auto half = chip(true, true, 1, 0.5f, true);
    bool scaled = true;
    for (size_t i = 0; i < half.size(); ++i) // integer sums, rounded once: within one step of 16 bits
        scaled &= std::abs(half[i] - (music[i] + 0.5f * effects[i])) <= 1.0f / 32768.0f;
    check(scaled, "effects at half");

    // Header LFO against the same registers written explicitly. No ratio:
    // the buffers either match or they do not. Each voice is built twice.
    constexpr int kFrames = 1024;
    auto lfo = [&](uint8_t sample, int pitch, int tremolo) {
        snd::MultiPcm pcm(10000000, r.data(), uint32_t(r.size()));
        key_on(pcm, 0, 0x20, 0, false, sample);
        if (pitch >= 0) write_lfo(pcm, uint8_t(pitch), uint8_t(tremolo));
        std::vector<float> out(size_t(kFrames) * 2);
        pcm.generate(out.data(), out.data() + kFrames, kFrames);
        return out;
    };
    const auto quiet = lfo(0, -1, -1);
    const auto again = lfo(0, -1, -1);
    const auto from_header = lfo(1, -1, -1);
    const auto header_again = lfo(1, -1, -1);
    const auto explicit_both = lfo(0, kPitchLfo, kTremolo);
    const auto tremolo_only = lfo(0, kPitchOff, kTremolo);
    const auto pitch_only = lfo(0, kPitchLfo, 0);
    const auto header_pitch_off = lfo(1, kPitchOff, kTremolo);
    const auto header_tremolo_off = lfo(1, kPitchLfo, 0);
    auto audible = [](const std::vector<float> &wave) {
        for (float v : wave)
            if (v != 0.f) return true;
        return false;
    };
    check(quiet == again && from_header == header_again, "LFO renders are deterministic");
    check(audible(quiet) && audible(from_header) && audible(tremolo_only) && audible(pitch_only), "LFO voices are not silent");
    check(from_header != quiet, "header LFO changes the unmodulated wave");
    check(from_header == explicit_both, "header LFO matches the explicit registers");
    check(header_pitch_off == tremolo_only, "clearing pitch depth leaves the tremolo");
    check(header_tremolo_off == pitch_only, "clearing tremolo leaves the pitch LFO");
    check(header_pitch_off != from_header && header_tremolo_off != from_header, "clearing one LFO changes the wave");
    check(header_pitch_off != header_tremolo_off, "pitch LFO and tremolo are different");
    check(header_pitch_off != quiet && header_tremolo_off != quiet, "the remaining LFO is still active");
    if (failures) return 1;
    std::printf("test_multipcm_volumes: ok\n");
    return 0;
}
