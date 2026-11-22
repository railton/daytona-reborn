// Opt-in reference/native event audit; no ROM data is stored in this source.
// Exit success verifies note identities and native health, NOT exact waveform,
// envelope, voice-slot allocation, pitch transient, or event-time equivalence.
// Remaining scalar/time differences are explicitly reported for review.
#include "runtime/game_loop.h"
#include "runtime/native_sound_engine.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
rt::Inputs input_for(unsigned frame, bool attract) {
    rt::Inputs in;
    if (attract) return in;
    if ((frame >= 1200 && frame < 1210) || (frame >= 1240 && frame < 1250) || (frame >= 1280 && frame < 1290)) in.in0 &= ~1;
    if ((frame >= 1400 && frame < 1410) || (frame >= 1600 && frame < 1610) ||
        (frame >= 2000 && frame < 2010) || (frame >= 2400 && frame < 2410)) in.in0 &= ~0x10;
    if ((frame >= 1800 && frame < 1810) || (frame >= 2200 && frame < 2210)) in.in0 &= ~0x20;
    if (frame >= 1400) in.accel = 0xe0;
    return in;
}
struct Note { uint64_t frame; unsigned rom, bank, slot, sample; double hz, gain, pan; };
struct Levels {
    uint64_t samples = 0, nonzero = 0, overrange = 0, full_scale = 0;
    double peak = 0, squares = 0, clamped_squares = 0;
    void add(float sample) {
        if (!std::isfinite(sample)) throw std::runtime_error("non-finite audio sample");
        const double value = sample, magnitude = std::abs(value);
        ++samples;
        nonzero += value != 0;
        overrange += magnitude > 1;
        full_scale += magnitude >= 1;
        peak = std::max(peak, magnitude);
        squares += value * value;
        const double clamped = std::clamp(value, -1., 1.);
        clamped_squares += clamped * clamped;
    }
    double rms() const { return samples ? std::sqrt(squares / double(samples)) : 0; }
    void report(const char *backend, const char *segment) const {
        std::printf("levels backend=%s segment=%s samples=%llu nonzero=%llu peak=%.9g rms=%.9g clamped_rms=%.9g overrange=%llu full_scale=%llu\n",
            backend, segment, (unsigned long long)samples, (unsigned long long)nonzero,
            peak, rms(), samples ? std::sqrt(clamped_squares / double(samples)) : 0,
            (unsigned long long)overrange, (unsigned long long)full_scale);
    }
};
struct Oracle : snd::SoundBoard {
    using SoundBoard::SoundBoard;
    std::array<std::array<std::array<uint8_t, 11>, 28>, 2> regs{};
    std::array<std::array<unsigned, 28>, 2> loaded{};
    std::array<unsigned, 2> bank{}, slot{}, address{};
    std::vector<Note> notes;
    void write(uint32_t addr, uint16_t data) override {
        const uint8_t d = uint8_t(data);
        if (addr == 0xc50000 || addr == 0xc70000) bank[addr == 0xc70000] = data & 3;
        else if ((addr >= 0xc40001 && addr <= 0xc40005) || (addr >= 0xc60001 && addr <= 0xc60005)) {
            const unsigned rom = addr >= 0xc60001, offset = (addr & 7) >> 1;
            if (offset == 1) slot[rom] = (d & 7) == 7 ? 28 : (d & 31) - ((d & 31) >> 3);
            else if (offset == 2) address[rom] = d;
            else if (slot[rom] < 28 && address[rom] < 11) {
                auto &r = regs[rom][slot[rom]];
                r[address[rom]] = d;
                if (address[rom] == 1) loaded[rom][slot[rom]] = d | ((r[2] & 1) << 8);
                if (address[rom] == 4 && (d & 128)) {
                    const unsigned fraction = ((r[3] & 15) << 6) | (r[2] >> 2);
                    int oct = ((r[3] >> 4) - 1) & 15; if (oct >= 8) oct -= 16;
                    const unsigned pan = r[0] >> 4, level = r[5] >> 1;
                    double l = 1, right = 1;
                    if (pan == 8) l = right = 0;
                    else if (pan & 8) right = 16 - pan == 7 ? 0 : std::pow(10., -.15 * (16 - pan));
                    else if (pan) l = pan == 7 ? 0 : std::pow(10., -.15 * pan);
                    notes.push_back({instructions() * 48000 / kIps, rom, bank[rom], slot[rom], loaded[rom][slot[rom]],
                        std::ldexp((10000000. / 224.) * (1024 + fraction) / 1024., oct),
                        .25 * std::pow(10., -.01875 * level) * std::hypot(l, right),
                        l + right ? std::atan2(right, l) * (4. / 3.14159265358979323846) - 1 : 0});
                }
            }
        }
        SoundBoard::write(addr, data);
    }
};
void event(void *context, const snd::NativeSoundSequencer::VoiceEvent &e) {
    if (e.kind == snd::NativeSoundSequencer::EventKind::NoteOn)
        static_cast<std::vector<Note> *>(context)->push_back({e.frame, e.rom, e.bank, unsigned(e.voice_id % 28),
            e.sample_index, e.source_rate_hz, e.gain, e.pan});
}
void dump(const std::string &path, const std::vector<Note> &notes) {
    FILE *out = std::fopen(path.c_str(), "w");
    if (!out) throw std::runtime_error("cannot write " + path);
    std::fprintf(out, "frame,rom,bank,slot,sample,hz,gain,pan\n");
    for (const auto &n : notes) std::fprintf(out, "%llu,%u,%u,%u,%u,%.12g,%.12g,%.12g\n",
        (unsigned long long)n.frame, n.rom, n.bank, n.slot, n.sample, n.hz, n.gain, n.pan);
    std::fclose(out);
}
constexpr uint64_t kWindowFrames = 4800; // inclusive, same 100 ms burst the strict audit already uses

uint64_t frame_delta(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Maximum-cardinality pairing of already sorted frames. The earliest feasible
// partner is taken; a fixed inclusive window makes that optimal.
struct WindowAudit {
    size_t unmatched_reference = 0, unmatched_native = 0, paired = 0;
    size_t pitch_mismatch = 0, gain_mismatch = 0, pan_mismatch = 0;
    uint64_t max_delta_frames = 0;
};

struct NoteId {
    unsigned rom, bank, sample;
    bool operator<(const NoteId &o) const {
        if (rom != o.rom) return rom < o.rom;
        if (bank != o.bank) return bank < o.bank;
        return sample < o.sample;
    }
};

WindowAudit audit_window(const std::vector<Note> &reference, const std::vector<Note> &native, uint64_t limit) {
    std::map<NoteId, std::pair<std::vector<Note>, std::vector<Note>>> groups;
    for (const auto &n : reference) groups[{n.rom, n.bank, n.sample}].first.push_back(n);
    for (const auto &n : native) groups[{n.rom, n.bank, n.sample}].second.push_back(n);
    const auto by_frame = [](const Note &a, const Note &b) { return a.frame < b.frame; };
    WindowAudit out;
    for (auto &group : groups) {
        auto &refs = group.second.first;
        auto &nats = group.second.second;
        std::stable_sort(refs.begin(), refs.end(), by_frame);
        std::stable_sort(nats.begin(), nats.end(), by_frame);
        size_t i = 0, j = 0, matched = 0;
        while (i < refs.size() && j < nats.size()) {
            const uint64_t delta = frame_delta(refs[i].frame, nats[j].frame);
            if (delta <= limit) {
                ++matched;
                out.pitch_mismatch += std::abs(refs[i].hz - nats[j].hz) > .001;
                out.gain_mismatch += std::abs(refs[i].gain - nats[j].gain) > .00001;
                out.pan_mismatch += std::abs(refs[i].pan - nats[j].pan) > .00001;
                out.max_delta_frames = std::max(out.max_delta_frames, delta);
                ++i;
                ++j;
            } else if (nats[j].frame < refs[i].frame) {
                ++j;
            } else {
                ++i;
            }
        }
        out.paired += matched;
        out.unmatched_reference += refs.size() - matched;
        out.unmatched_native += nats.size() - matched;
    }
    return out;
}

int self_test_window() {
    // Same onsets stored out of order: grouping and the frame sort must still pair both.
    const std::vector<Note> crossed_ref{{1140, 0, 2, 0, 9, 100, 0.5, 0}, {1000, 0, 2, 0, 9, 100, 0.5, 0}};
    const std::vector<Note> crossed_nat{{1050, 0, 2, 0, 9, 100, 0.5, 0}, {910, 0, 2, 0, 9, 100, 0.5, 0}};
    const auto crossed = audit_window(crossed_ref, crossed_nat, 100);
    if (crossed.paired != 2 || crossed.unmatched_reference || crossed.unmatched_native || crossed.max_delta_frames != 90 ||
        crossed.pitch_mismatch || crossed.gain_mismatch || crossed.pan_mismatch) {
        std::fprintf(stderr, "FAIL: sorted window pairs=%zu max_delta=%llu\n", crossed.paired,
                     (unsigned long long)crossed.max_delta_frames);
        return 1;
    }
    // Equal time, different gain and pan: still one identity pair. The scalars stay mismatches.
    const Note same{1000, 0, 1, 0, 7, 440, 0.5, -0.5};
    const Note shifted{1000, 0, 1, 0, 7, 440, 0.25, 0};
    const auto scalar = audit_window({same}, {shifted}, kWindowFrames);
    if (scalar.paired != 1 || scalar.unmatched_reference || scalar.unmatched_native || scalar.max_delta_frames != 0 ||
        scalar.pitch_mismatch != 0 || scalar.gain_mismatch != 1 || scalar.pan_mismatch != 1) {
        std::fprintf(stderr, "FAIL: scalar identity pair gain=%zu pan=%zu\n", scalar.gain_mismatch, scalar.pan_mismatch);
        return 1;
    }
    const Note edge{0, 0, 0, 0, 1, 1, 0, 0};
    const Note at_limit{kWindowFrames, 0, 0, 0, 1, 1, 0, 0};
    const Note past_limit{kWindowFrames + 1, 0, 0, 0, 1, 999, 1, 1};
    const auto inside = audit_window({edge}, {at_limit}, kWindowFrames);
    const auto outside = audit_window({edge}, {past_limit}, kWindowFrames);
    if (inside.paired != 1 || inside.max_delta_frames != kWindowFrames || inside.pitch_mismatch || inside.gain_mismatch ||
        inside.pan_mismatch || outside.paired || outside.pitch_mismatch || outside.gain_mismatch || outside.pan_mismatch ||
        outside.unmatched_reference != 1 || outside.unmatched_native != 1 || outside.max_delta_frames) {
        std::fprintf(stderr, "FAIL: inclusive %llu-frame window\n", (unsigned long long)kWindowFrames);
        return 1;
    }
    const auto only_ref = audit_window({edge}, {}, kWindowFrames);
    const auto only_nat = audit_window({}, {edge}, kWindowFrames);
    const Note other_sample{0, 0, 0, 0, 2, 1, 0, 0};
    const Note other_rom{0, 1, 0, 0, 1, 1, 0, 0};
    const auto split = audit_window({edge}, {other_sample, other_rom}, kWindowFrames);
    if (only_ref.unmatched_reference != 1 || only_ref.unmatched_native || only_nat.unmatched_native != 1 || only_nat.unmatched_reference ||
        split.paired || split.unmatched_reference != 1 || split.unmatched_native != 2) {
        std::fprintf(stderr, "FAIL: one-sided window counts\n");
        return 1;
    }
    std::printf("window_self_test pairs=2 gain_mismatch=1 pan_mismatch=1 inclusive_frames=%llu\n",
                (unsigned long long)kWindowFrames);
    std::printf("window_self_test: ok\n");
    return 0;
}
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test-window") return self_test_window();
    const bool attract = argc == 5 && std::string(argv[4]) == "attract";
    std::optional<rt::RaceCourse> native_course;
    if (argc == 6 && std::string(argv[4]) == "--native-course") {
        const std::string selection = argv[5];
        if (selection.size() == 1 && selection.front() >= '1' && selection.front() <= '3')
            native_course = static_cast<rt::RaceCourse>(selection.front() - '1');
    }
    if (argc != 4 && !attract && !native_course) {
        std::fprintf(stderr, "usage: native_oracle ROM_DIR FRAMES OUTPUT_PREFIX [attract | --native-course 1|2|3]\n");
        return 2;
    }
    try {
        const std::string dir = argv[1], prefix = argv[3];
        const unsigned count = std::strtoul(argv[2], nullptr, 10);
        auto program = load(dir + "/sound_program.bin"), pcm1 = load(dir + "/pcm1.bin"), pcm2 = load(dir + "/pcm2.bin");
        auto oracle = std::make_unique<Oracle>(program, pcm1, pcm2);
        snd::NativeSoundSequencer sequence(program);
        std::vector<Note> notes;
        sequence.set_sink(event, &notes);
        snd::NativeSoundEngine engine(program, pcm1, pcm2);
        rt::GameLoop game(dir, false);
        if (game.sound()) throw std::runtime_error("disabled GameLoop constructed reference sound");
        if (const char *nv = std::getenv("M2_NVRAM")) { // the app's saved EEPROM and backup RAM
            auto into = [&](const std::string &file, auto &dst) {
                const auto d = load(std::string(nv) + "/" + file);
                if (d.size() == dst.size()) std::copy(d.begin(), d.end(), dst.begin());
            };
            into("ioboard_eeprom.bin", game.board().io().eeprom);
            into("backup_ram.bin", game.board().backup_ram());
        }
        if (native_course && game.request_race({*native_course}) != rt::RaceStartResult::Accepted)
            throw std::runtime_error("native race request rejected");
        game.board().video().set_external_3d(true);
        unsigned guest_start_tick = 0;
        uint64_t rendered = 0, nonzero = 0, fm_nonzero = 0, bytes_total = 0;
        // This timer includes audit/metering work; it is not a production benchmark.
        double render_cpu = 0, peak = 0;
        std::array<Levels, 3> reference_levels{}, native_levels{}, fm_levels{};
        std::array<uint64_t, 3> native_clipped{}, native_limited{};
        bool native_failed = false;
        for (unsigned frame = 0; frame < count; ++frame) {
            const size_t segment = frame < 1400 ? 1 : 2;
            rt::Inputs inputs;
            if (native_course)
                inputs.accel = 0xe0;
            else
                inputs = input_for(frame, attract);
            game.run_frame(inputs);
            if (native_course && !guest_start_tick && game.race_status() == rt::RaceStatus::Racing) {
                guest_start_tick = frame + 1;
                std::printf("guest_race_start_tick=%u\n", guest_start_tick);
            }
            const auto bytes = game.board().take_sound_bytes(); bytes_total += bytes.size();
            oracle->send(bytes.data(), bytes.size());
            oracle->advance(1. / rt::GameLoop::kFrameHz);
            for (float value : oracle->take_fm()) {
                fm_nonzero += value != 0;
                fm_levels[0].add(value); fm_levels[segment].add(value);
            }
            for (float value : oracle->take_pcm()) {
                reference_levels[0].add(value); reference_levels[segment].add(value);
            }
            const uint64_t due = uint64_t((frame + 1) * 48000. / rt::GameLoop::kFrameHz);
            sequence.send(bytes.data(), bytes.size());
            sequence.advance(size_t(due - rendered));
            if (!native_failed) try {
                const auto start = std::clock();
                const auto stats_before = engine.stats();
                engine.send(bytes.data(), bytes.size());
                std::array<float, 2048> output;
                uint64_t remaining = due - rendered;
                while (remaining) {
                    const size_t n = size_t(std::min<uint64_t>(remaining, 1024));
                    engine.render(output.data(), n);
                    for (size_t i = 0; i < n * 2; ++i) {
                        if (!std::isfinite(output[i]) || std::abs(output[i]) > 1) throw std::runtime_error("unbounded native sample");
                        nonzero += output[i] != 0; peak = std::max(peak, double(std::abs(output[i])));
                        native_levels[0].add(output[i]); native_levels[segment].add(output[i]);
                    }
                    remaining -= n;
                }
                const auto stats_after = engine.stats();
                const uint64_t clipped = stats_after.clipped - stats_before.clipped;
                const uint64_t limited = stats_after.limited_frames - stats_before.limited_frames;
                native_clipped[0] += clipped; native_clipped[segment] += clipped;
                native_limited[0] += limited; native_limited[segment] += limited;
                render_cpu += double(std::clock() - start) / CLOCKS_PER_SEC;
            } catch (const std::exception &e) { std::fprintf(stderr, "native fault at game frame %u: %s\n", frame, e.what()); native_failed = true; }
            rendered = due;
        }
        // Rates differ, so RMS is normalized by each backend's sample count.
        // This is an output-level audit, not waveform or perceived-loudness parity.
        const char *segments[] = {"total", "before_1400", "from_1400"};
        for (size_t i = 0; i < reference_levels.size(); ++i) {
            reference_levels[i].report("reference_pcm", segments[i]);
            fm_levels[i].report("reference_fm", segments[i]);
            native_levels[i].report("native", segments[i]);
            std::printf("level_comparison segment=%s reference_pcm_to_native_rms=%.9g native_mixer_clipped=%llu native_limited_frames=%llu\n",
                segments[i], native_levels[i].rms() ? reference_levels[i].rms() / native_levels[i].rms() : 0,
                (unsigned long long)native_clipped[i], (unsigned long long)native_limited[i]);
        }
        dump(prefix + "-reference.csv", oracle->notes); dump(prefix + "-native.csv", notes);
        size_t compared = 0, identity = 0, pitch = 0, gain = 0, pan = 0, reordered = 0;
        uint64_t maximum_timing_delta = 0;
        auto same_identity = [](const Note &a, const Note &b) { return a.sample == b.sample && a.bank == b.bank; };
        auto same_scalar = [&](const Note &a, const Note &b) {
            return same_identity(a, b) && std::abs(a.hz - b.hz) <= .001 &&
                std::abs(a.gain - b.gain) <= .00001 && std::abs(a.pan - b.pan) <= .00001;
        };
        for (unsigned rom = 0; rom < 2; ++rom) {
            std::vector<Note> a, b;
            for (const auto &n : oracle->notes) if (n.rom == rom) a.push_back(n);
            for (const auto &n : notes) if (n.rom == rom) b.push_back(n);
            std::printf("rom=%u reference_notes=%zu native_notes=%zu\n", rom, a.size(), b.size());
            std::vector<size_t> pairs(a.size(), b.size());
            std::vector<bool> used(b.size(), false);
            // Keep already exact ordered pairs fixed. Matching repeated notes
            // by nearest timestamp alone can wrongly swap a stereo music pair.
            for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
                if (same_scalar(a[i], b[i])) pairs[i] = i, used[i] = true;
            for (size_t i = 0; i < a.size(); ++i) {
                if (pairs[i] != b.size()) continue;
                size_t best = b.size();
                uint64_t distance = UINT64_MAX;
                for (size_t j = 0; j < b.size(); ++j) {
                    if (used[j] || !same_identity(a[i], b[j])) continue;
                    // Restrict reordering to the same local 100 ms command
                    // burst; do not accept an unrelated missing/extra sound.
                    const uint64_t delta = a[i].frame > b[j].frame ? a[i].frame - b[j].frame : b[j].frame - a[i].frame;
                    if (delta > 4800 || std::abs(a[i].gain - b[j].gain) > .00001 ||
                        std::abs(a[i].pan - b[j].pan) > .00001) continue;
                    if (delta < distance) { distance = delta; best = j; }
                }
                if (best != b.size()) { pairs[i] = best; used[best] = true; reordered += best != i; }
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (pairs[i] == b.size()) { ++identity; continue; }
                const auto &x = a[i], &y = b[pairs[i]];
                ++compared;
                pitch += std::abs(x.hz - y.hz) > .001;
                gain += std::abs(x.gain - y.gain) > .00001;
                pan += std::abs(x.pan - y.pan) > .00001;
                const auto delta = x.frame > y.frame ? x.frame - y.frame : y.frame - x.frame;
                maximum_timing_delta = std::max(maximum_timing_delta, delta);
                if (!same_scalar(x, y))
                    std::printf("scalar difference rom=%u sample=%u ref_hz=%.9g native_hz=%.9g ref_gain=%.9g native_gain=%.9g ref_pan=%.9g native_pan=%.9g\n",
                        rom, x.sample, x.hz, y.hz, x.gain, y.gain, x.pan, y.pan);
            }
            identity += std::count(used.begin(), used.end(), false);
        }
        std::printf("reordered_notes=%zu maximum_timing_delta_frames=%llu maximum_timing_delta_ms=%.6f (reported, not exact-timing parity)\n",
            reordered, (unsigned long long)maximum_timing_delta, double(maximum_timing_delta) / 48.);
        const auto stats = sequence.stats(); const auto mix = engine.stats();
        std::printf("frames=%u bytes=%llu notes_ref=%zu notes_native=%zu compared=%zu identity_mismatch=%zu pitch_mismatch=%zu gain_mismatch=%zu pan_mismatch=%zu\n",
            count, (unsigned long long)bytes_total, oracle->notes.size(), notes.size(), compared, identity, pitch, gain, pan);
        std::printf("sequence messages=%llu events=%llu unsupported=%llu invalid=%llu limits=%llu native_failed=%d mixer_invalid=%llu output_nonzero=%llu peak=%g render_cpu_ms=%g fm_nonzero=%llu\n",
            (unsigned long long)stats.messages, (unsigned long long)stats.sequence_events, (unsigned long long)stats.unsupported,
            (unsigned long long)stats.invalid_data, (unsigned long long)stats.event_limit_hits, native_failed, (unsigned long long)mix.invalid,
            (unsigned long long)nonzero, peak, render_cpu * 1000, (unsigned long long)fm_nonzero);
        const WindowAudit window = audit_window(oracle->notes, notes, kWindowFrames);
        std::printf("window_audit unmatched_reference=%zu unmatched_native=%zu paired=%zu pitch_mismatch=%zu gain_mismatch=%zu pan_mismatch=%zu max_delta_frames=%llu\n",
            window.unmatched_reference, window.unmatched_native, window.paired, window.pitch_mismatch, window.gain_mismatch,
            window.pan_mismatch, (unsigned long long)window.max_delta_frames);
        if (native_course && !guest_start_tick) {
            std::fprintf(stderr, "native race did not start during the capture\n");
            return 1;
        }
        return native_failed || sequence.failed() || mix.invalid || stats.unsupported || identity ||
            notes.size() != oracle->notes.size() || (count >= 1200 && !nonzero);
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
