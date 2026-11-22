// m2run: the recompiled game running on its own on the native board: no
// trace, no MAME, no emulated CPU and no instruction clock. Headless for now:
// frames go to raw dumps (scripts/rgb2png.py converts them).
//
//   m2run IMAGES_DIR FRAMES [--inputs scripts/inputs/X.txt] [--dump DIR --every N] [--wav FILE]
//         [--aspect W:H [--hud-edges] [--stretch-backdrop]] [--draw-distance N] [--frame-skip N]
//         [--dump-from FRAME] [--native-audio-check] [--nvram DIR] [--save-nvram DIR] [--pc-solo 1|2|3]
//         [--race-status-log FILE] (native session transitions, no guest RAM edits)
//         [--snapshot-stamps] [--scene-stamps] (completed tick observations on stdout)
//         [--scene-pair TICK] (own and verify two adjacent completed draws)
//         [--transmission automatic|manual] [--race-mode normal|time-attack] (requires --pc-solo)
//         [--link-listen PORT --link-next HOST:PORT [--link-sync]]
//         [--draw-budget N] [--draw-order-only] [--original-selection] [--scenery-log FILE]
//         [--omit-scenery-cell TICK:CELL]
//         [--rom-object-counts FILE] (pre-cull ROM geometry counts)
//
// --draw-budget is an override independent of --draw-distance:
// 0 uses Automatic; e.g. --draw-distance 1 --draw-budget 5000 widens
// cell selection while retaining the original budget. The launcher exposes
// the same Automatic/Custom policy.
// --draw-order-only retains the original cell membership but orders it as
// the positive --draw-distance would; combine with --draw-budget 5000.
// --original-selection disables only the new widescreen selection, for comparison.
// --scenery-log writes JSON lines for EVERY frame at/after --dump-from, even
// without image capture. Costs are game metadata, not visible polygon counts.
// --omit-scenery-cell filters the list when its hook runs on zero-based TICK.
// The game can retain the filtered RAM list beyond that tick. Inspect the
// resulting trace instead of assuming how many rendered frames it affects.
// --nvram DIR starts from the app's saved settings EEPROM and backup RAM
// (tools/common/nvram.h). --link-listen/--link-next: link play (the
// communication board, Revision A) over TCP, as the app does: listen for the
// cabinet before this one, connect to the next; --link-sync holds each frame
// to the master's. The link's state is printed at the end.
// --native-audio-check: no reference sound board; the game's sound commands
// go to the native audio engine as the app's would, and the first fault
// stops the run with its frame and that frame's command bytes.
// --aspect widens the screen (the widescreen enhancement, e.g. 16:9); dumps
// are then wider than 496 (the width is printed).
// --wav writes the sound board's output (YM3438 + both MultiPCMs, mixed at
// 48 kHz, 16-bit stereo).
//
// Frame pacing is the game's own: vblank starts when the game has finished
// its frame and waits in its idle loop (or after a cap, for frames that never
// idle), and ends when the vblank handler has returned. A windowed build
// waits for the display's vsync at that point; the frame rate is the only
// limit.

#include "runtime/game_loop.h"
#include "runtime/msc2_snapshot.h"
#include "runtime/race_request.h"
#include "runtime/rom_import.h"
#include "runtime/snapshot_timing.h"
#include "../common/input_script.h"
#include "runtime/native_sound_engine.h"
#include "runtime/enhance_diagnostics.h"
#include "../common/nvram.h"
#include "../common/scenery_log.h"
#include "../common/race_start_failure.h"
#include "app/link_socket.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

struct RomObjectCounts {
    rt::M2Board &board;
    std::ofstream out;

    static void write(void *context, const rt::RomObjectEmission &object) {
        auto &self = *static_cast<RomObjectCounts *>(context);
        unsigned vertices = 0;
        for (const auto &primitive : object.primitives) vertices += primitive.vertex_count;
        self.out << self.board.frame() << ',' << object.command_word << ',' << object.oba << ','
                 << object.obc << ',' << unsigned(object.mode) << ',' << object.tpa << ','
                 << object.tha << ',' << object.primitives.size() << ',' << vertices << '\n';
    }
};

const char *race_status_name(rt::RaceStatus status) {
    switch (status) {
    case rt::RaceStatus::Idle: return "idle";
    case rt::RaceStatus::Starting: return "starting";
    case rt::RaceStatus::Racing: return "racing";
    case rt::RaceStatus::Finished: return "finished";
    case rt::RaceStatus::WrongCourse: return "wrong_course";
    case rt::RaceStatus::WrongMode: return "wrong_mode";
    case rt::RaceStatus::WrongTransmission: return "wrong_transmission";
    case rt::RaceStatus::TimedOut: return "timed_out";
    case rt::RaceStatus::Cancelled: return "cancelled";
    }
    throw std::runtime_error("unknown native race status");
}

// Linear resampling of interleaved stereo to `rate`, added into out.
void mix_into(std::vector<float> &out, const std::vector<float> &in, double in_rate, double rate) {
    const size_t frames_in = in.size() / 2;
    if (frames_in < 2) return;
    const size_t frames_out = size_t(double(frames_in - 1) * rate / in_rate);
    if (out.size() < frames_out * 2) out.resize(frames_out * 2, 0.0f);
    for (size_t i = 0; i < frames_out; ++i) {
        const double pos = double(i) * in_rate / rate;
        const size_t k = size_t(pos);
        const float f = float(pos - double(k));
        for (int ch = 0; ch < 2; ++ch) out[i * 2 + ch] += in[k * 2 + ch] * (1 - f) + in[(k + 1) * 2 + ch] * f;
    }
}

void write_wav(const std::string &path, const std::vector<float> &mix, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    const uint32_t bytes = uint32_t(mix.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : mix) u16(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2run IMAGES_DIR FRAMES [--inputs FILE] [--dump DIR --every N] [--wav FILE] "
                             "[--aspect W:H]\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path, wav_path, nvram_dir, save_nvram_dir, link_next, scenery_log_path, race_log_path;
    std::string rom_object_counts_path;
    int link_listen = 0;
    std::optional<rt::StartRaceRequest> solo;
    std::optional<rt::RaceTransmission> transmission;
    std::optional<rt::RaceMode> race_mode;
    bool link_sync = false, native_check = false;
    bool snapshot_stamps = false, scene_stamps = false;
    std::optional<uint64_t> scene_pair_from;
    std::optional<std::pair<uint64_t, uint8_t>> omit_cell;
    uint64_t every = 0, dump_from = 0;
    double aspect = 0;
    int frame_skip = 0;
    bool hud_edges = false, stretch_backdrop = false, original_selection = false;
    uint32_t draw_budget = 0;
    for (int i = 3; i < argc; i++) {
        if ((!std::strcmp(argv[i], "--draw-budget") || !std::strcmp(argv[i], "--pc-solo") ||
             !std::strcmp(argv[i], "--race-status-log") || !std::strcmp(argv[i], "--transmission") ||
             !std::strcmp(argv[i], "--scene-pair") || !std::strcmp(argv[i], "--omit-scenery-cell") ||
             !std::strcmp(argv[i], "--race-mode") ||
             !std::strcmp(argv[i], "--rom-object-counts")) && i + 1 == argc) {
            std::fprintf(stderr, "m2run: %s needs a value\n", argv[i]);
            return 2;
        }
        if (!std::strcmp(argv[i], "--hud-edges")) hud_edges = true;
        if (!std::strcmp(argv[i], "--stretch-backdrop")) stretch_backdrop = true;
        if (!std::strcmp(argv[i], "--link-sync")) link_sync = true;
        if (!std::strcmp(argv[i], "--native-audio-check")) native_check = true;
        if (!std::strcmp(argv[i], "--draw-order-only")) rt::EnhanceDiagnostics::draw_order_only = true;
        if (!std::strcmp(argv[i], "--original-selection")) original_selection = true;
    }
    for (int i = 3; i < argc; i += 2) {
        if (!std::strcmp(argv[i], "--snapshot-stamps") || !std::strcmp(argv[i], "--scene-stamps")) {
            if (!std::strcmp(argv[i], "--snapshot-stamps")) snapshot_stamps = true;
            else scene_stamps = true;
            i--;
            continue;
        }
        if (i + 1 == argc) break;
        if (!std::strcmp(argv[i], "--hud-edges") || !std::strcmp(argv[i], "--stretch-backdrop") ||
            !std::strcmp(argv[i], "--link-sync") || !std::strcmp(argv[i], "--native-audio-check") ||
            !std::strcmp(argv[i], "--draw-order-only") || !std::strcmp(argv[i], "--original-selection")) { i--; continue; }
        if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--rom-object-counts")) rom_object_counts_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--dump-from")) dump_from = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--wav")) wav_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--save-nvram")) save_nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--link-listen")) link_listen = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--link-next")) link_next = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-distance")) rt::GameLoop::set_draw_distance(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--draw-budget")) {
            if (!rt::parse_scenery_budget(argv[i + 1], draw_budget)) {
                std::fprintf(stderr, "m2run: --draw-budget needs 0 (Automatic) or 1..1000000\n");
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--scene-pair")) {
            const std::string_view value(argv[i + 1]);
            uint64_t tick = 0;
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, tick);
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                std::fprintf(stderr, "m2run: --scene-pair needs a decimal starting tick\n");
                return 2;
            }
            scene_pair_from = tick;
        }
        else if (!std::strcmp(argv[i], "--scenery-log")) scenery_log_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--omit-scenery-cell")) {
            const std::string_view value(argv[i + 1]);
            const auto split = value.find(':');
            uint64_t tick = 0;
            unsigned cell = 0;
            if (split != std::string_view::npos) {
                const auto end = value.data() + value.size();
                const auto separator = value.data() + split;
                const auto a = std::from_chars(value.data(), separator, tick);
                const auto b = std::from_chars(separator + 1, end, cell);
                if (a.ec == std::errc{} && a.ptr == separator && b.ec == std::errc{} &&
                    b.ptr == end && cell <= 255) {
                    omit_cell.emplace(tick, uint8_t(cell));
                    continue;
                }
            }
            std::fprintf(stderr, "m2run: --omit-scenery-cell needs a decimal tick and cell (TICK:0..255)\n");
            return 2;
        }
        else if (!std::strcmp(argv[i], "--race-status-log")) race_log_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--transmission")) {
            transmission = rt::parse_race_transmission(argv[i + 1]);
            if (!transmission) {
                std::fprintf(stderr, "m2run: --transmission expects automatic or manual\n");
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--race-mode")) {
            race_mode = rt::parse_race_mode(argv[i + 1]);
            if (!race_mode) {
                std::fprintf(stderr, "m2run: --race-mode expects normal or time-attack\n");
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--pc-solo")) {
            const std::string course = argv[i + 1];
            if (course != "1" && course != "2" && course != "3") {
                std::fprintf(stderr, "m2run: --pc-solo expects 1, 2 or 3\n");
                return 2;
            }
            solo = rt::StartRaceRequest{rt::RaceCourse(course[0] - '1')};
        }
        else if (!std::strcmp(argv[i], "--frame-skip")) frame_skip = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--aspect")) {
            double w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%lf:%lf", &w, &h) == 2 && h > 0) aspect = w / h;
        }
    }

    if (snapshot_stamps || scene_stamps || scene_pair_from) {
        const std::string_view value(argv[2]);
        const auto end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, frames);
        if (parsed.ec != std::errc{} || parsed.ptr != end || frames == 0) {
            std::fprintf(stderr, "m2run: %s needs a positive decimal frame count\n",
                         snapshot_stamps ? "--snapshot-stamps" : scene_stamps ? "--scene-stamps" : "--scene-pair");
            return 2;
        }
    }
    if (scene_pair_from && (frames < 2 || *scene_pair_from > frames - 2)) {
        std::fprintf(stderr, "m2run: --scene-pair needs two ticks inside the run\n");
        return 2;
    }
    if (transmission && !solo) {
        std::fprintf(stderr, "m2run: --transmission requires --pc-solo\n");
        return 2;
    }
    if (race_mode && !solo) {
        std::fprintf(stderr, "m2run: --race-mode requires --pc-solo\n");
        return 2;
    }
    if (transmission) solo->transmission = *transmission;
    if (race_mode) solo->mode = *race_mode;
    const bool original_cell_selection = rt::Enhance::draw_distance == 0 ||
        (rt::EnhanceDiagnostics::draw_order_only && rt::Enhance::draw_distance > 0);
    if (omit_cell && (omit_cell->first >= frames || !original_cell_selection || aspect != 0)) {
        std::fprintf(stderr, "m2run: --omit-scenery-cell needs an in-range tick, original aspect and no selection expansion\n");
        return 2;
    }
    try {
        if (rt::EnhanceDiagnostics::draw_order_only && rt::Enhance::draw_distance <= 0)
            throw std::runtime_error("--draw-order-only needs positive --draw-distance");
        tools::SceneryLog scenery_log(scenery_log_path);
        rt::GameLoop game(dir, !native_check);
        RomObjectCounts rom_objects{game.board(), {}};
        if (!rom_object_counts_path.empty()) {
            rom_objects.out.exceptions(std::ios::failbit | std::ios::badbit);
            rom_objects.out.open(rom_object_counts_path, std::ios::binary);
            rom_objects.out << "frame,command_word,oba,obc,mode,tpa,tha,primitives,vertices\n";
            game.board().set_rom_object_sink(RomObjectCounts::write, &rom_objects);
        }
        game.set_draw_budget(draw_budget);
        game.board().scenery()->original_selection = original_selection;
        scenery_log.attach(game);
        std::unique_ptr<snd::NativeSoundEngine> native;
        if (native_check) {
            auto file = [&](const char *name) {
                std::ifstream f(dir + "/" + name, std::ios::binary);
                return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), {}};
            };
            native = std::make_unique<snd::NativeSoundEngine>(file("sound_program.bin"), file("pcm1.bin"), file("pcm2.bin"));
        }
        std::vector<float> native_out;
        uint64_t native_rendered = 0;
        if (!nvram_dir.empty()) tools::load_nvram(game, nvram_dir);
        if (solo) {
            if (std::strcmp(rt::rom_set_name(), "daytona") || link_listen || !link_next.empty())
                throw std::runtime_error("--pc-solo requires Revision A with the network disabled");
            if (game.request_race(*solo) != rt::RaceStartResult::Accepted)
                throw std::runtime_error("native race request rejected");
        }
        std::ofstream race_log;
        if (!race_log_path.empty()) {
            race_log.exceptions(std::ios::failbit | std::ios::badbit);
            race_log.open(race_log_path);
        }
        auto last_race_status = game.race_status();
        const auto log_race = [&](const char *event) {
            race_log << "{\"event\":\"" << event << "\",\"frame\":" << game.frames()
                     << ",\"status\":\"" << race_status_name(game.race_status()) << "\"}\n";
            race_log.flush(); // A successful diagnostic must not silently lose its evidence.
        };
        if (race_log.is_open()) log_race("begin");
        std::unique_ptr<app::TcpLink> link;
        if (link_listen > 0 || !link_next.empty()) {
            link = std::make_unique<app::TcpLink>(uint16_t(link_listen), link_next);
            if (!link->error().empty()) throw std::runtime_error("link: " + link->error());
            game.board().set_link(link.get(), link_sync);
        }
        game.set_frame_skip(frame_skip);
        if (aspect > 0) {
            game.set_aspect(aspect);
            game.set_hud_edges(hud_edges);
            game.set_stretch_backdrop(stretch_backdrop);
            std::printf("m2run: screen %dx%d\n", game.screen_width(), rt::GameLoop::kHeight);
        }
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        const auto t0 = std::chrono::steady_clock::now();
        if (snapshot_stamps && std::puts("snapshot_stamp,tick,time_ns,discontinuity_before") < 0)
            throw std::runtime_error("snapshot stamp header write failed");
        if (scene_stamps && std::puts("scene_stamp,tick,time_ns,discontinuity_before,last_drawn_tick") < 0)
            throw std::runtime_error("scene stamp header write failed");
        std::vector<float> fm, pcm;
        std::array<std::optional<rt::M2Board::DrawnScene>, 2> scene_pair;
        uint64_t drive_commands = 0, drive_kinds[16] = {}; // the force feedback drive board's commands, by type
        auto &scenery = *game.board().scenery();
        for (uint64_t f = 0; f < frames; f++) {
            const bool omit_this_tick = omit_cell && f == omit_cell->first;
            if (omit_this_tick) scenery.omit_cell = omit_cell->second;
            game.run_frame(script.at(game.board().frame()));
            // Diagnostic (divergence search): M2_RAM_HASH=FILE.csv writes, per
            // frame from M2_RAM_HASH_FROM, an FNV-1a hash of each 4 KB of work
            // RAM (0x500000..0x5fffff) and of each 4 KB of 0x200000..0x21ffff.
            {
                static std::FILE *ram_hash = [] {
                    const char *p = std::getenv("M2_RAM_HASH");
                    return p && *p ? std::fopen(p, "w") : nullptr;
                }();
                static const uint64_t ram_hash_from = [] {
                    const char *v = std::getenv("M2_RAM_HASH_FROM");
                    return v ? std::strtoull(v, nullptr, 10) : 0ull;
                }();
                if (ram_hash && f >= ram_hash_from) {
                    std::fprintf(ram_hash, "%" PRIu64, f);
                    auto chunk = [&](uint32_t base, uint32_t chunks) {
                        for (uint32_t k = 0; k < chunks; ++k) {
                            uint64_t h = 1469598103934665603ull;
                            for (uint32_t a = base + k * 0x1000; a < base + (k + 1) * 0x1000; a += 4) {
                                h ^= game.board().read_dword(a);
                                h *= 1099511628211ull;
                            }
                            std::fprintf(ram_hash, ",%016" PRIx64, h);
                        }
                    };
                    chunk(0x500000u, 256);
                    chunk(0x200000u, 32);
                    std::fprintf(ram_hash, "\n");
                }
                // M2_RAM_DUMP=FILE with M2_RAM_DUMP_AT=N: the raw work RAM after frame N.
                static const char *ram_dump = std::getenv("M2_RAM_DUMP");
                static const uint64_t ram_dump_at = [] {
                    const char *v = std::getenv("M2_RAM_DUMP_AT");
                    return v ? std::strtoull(v, nullptr, 10) : ~0ull;
                }();
                if (ram_dump && *ram_dump && f == ram_dump_at) {
                    if (std::FILE *d = std::fopen(ram_dump, "wb")) {
                        for (uint32_t a = 0x500000u; a < 0x600000u; ++a) std::fputc(game.board().read_byte(a), d);
                        std::fclose(d);
                    }
                }
            }
            if (omit_this_tick) {
                scenery.omit_cell.reset();
                const auto &m = scenery.measured;
                if (!m.omit_hooks) throw std::runtime_error("scenery omission tick had no draw-list update");
                std::fprintf(stderr, "scenery_omission,%" PRIu64 ",%u,%u,%u\n",
                             f, unsigned(omit_cell->second), m.omit_hooks, m.omitted_cells);
            }
            if (scene_pair_from && f >= *scene_pair_from && f - *scene_pair_from < scene_pair.size()) {
                auto scene = game.board().copy_drawn_scene();
                if (!scene || scene->context.frame != f || game.frames() - 1 != f)
                    throw std::runtime_error("requested scene-pair tick was not drawn");
                scene_pair[f - *scene_pair_from] = std::move(scene);
            }
            if (snapshot_stamps || scene_stamps) {
                const auto elapsed = std::chrono::steady_clock::now() - t0;
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                // This process observes a single GameLoop. Trace ticks belong
                // to this run, irrespective of whether a frame was drawn.
                const auto stamp = rt::completed_snapshot_stamp(game.frames(), uint64_t(ns), 0);
                if (!stamp) throw std::runtime_error("no completed simulation tick");
                if (snapshot_stamps && std::printf("snapshot_stamp,%" PRIu64 ",%" PRIu64 ",%d\n",
                                                    stamp->tick, stamp->time_ns, stamp->discontinuity_before) < 0)
                    throw std::runtime_error("snapshot stamp write failed");
                if (scene_stamps) {
                    const auto drawn = game.board().last_drawn_tick();
                    if (drawn && *drawn > stamp->tick)
                        throw std::runtime_error("scene tick is ahead of the completed simulation");
                    const auto drawn_field = drawn ? std::to_string(*drawn) : std::string();
                    if (std::printf("scene_stamp,%" PRIu64 ",%" PRIu64 ",%d,%s\n", stamp->tick, stamp->time_ns,
                                    stamp->discontinuity_before, drawn_field.c_str()) < 0)
                        throw std::runtime_error("scene stamp write failed");
                }
            }
            if (race_log.is_open() && game.race_status() != last_race_status) {
                log_race("transition");
                last_race_status = game.race_status();
            }
            if (solo) {
                const auto status = game.race_status();
                tools::finalize_controlled_solo_failure(status, [&] {
                    if (race_log.is_open()) log_race("end");
                });
                if (status == rt::RaceStatus::Racing) {
                    std::printf("m2run: PC solo race started at frame %" PRIu64 "\n", game.frames());
                    solo.reset();
                }
            }
            for (uint8_t c : std::exchange(game.board().io().drive_commands, {})) ++drive_commands, ++drive_kinds[c >> 4];
            if (native) { // the app's native audio path, frame by frame
                const auto bytes = game.board().take_sound_bytes();
                const auto before = native->stats();
                try {
                    native->send(bytes.data(), bytes.size());
                    const uint64_t due = uint64_t(double(game.frames()) * 48000.0 / rt::GameLoop::kFrameHz);
                    native_out.resize(size_t(due - native_rendered) * 2);
                    if (due > native_rendered) native->render(native_out.data(), size_t(due - native_rendered));
                    native_rendered = due;
                } catch (const std::exception &e) {
                    std::fprintf(stderr, "m2run: native audio fault at frame %" PRIu64 ": %s\n", game.frames(), e.what());
                }
                const auto after = native->stats();
                if (after.invalid != before.invalid || after.unsupported != before.unsupported || native->sequence_stats().invalid_data) {
                    std::fprintf(stderr, "m2run: native audio at frame %" PRIu64 ": invalid %" PRIu64 " -> %" PRIu64
                                 ", unsupported %" PRIu64 " -> %" PRIu64 "; bytes this frame:",
                                 game.frames(), before.invalid, after.invalid, before.unsupported, after.unsupported);
                    for (uint8_t b : bytes) std::fprintf(stderr, " %02x", b);
                    std::fprintf(stderr, "\n");
                    break;
                }
            }
            if (!wav_path.empty() && game.sound()) {
                const auto a = game.sound()->take_fm(), b = game.sound()->take_pcm();
                fm.insert(fm.end(), a.begin(), a.end());
                pcm.insert(pcm.end(), b.begin(), b.end());
            }
            if (game.board().frame() >= dump_from)
                scenery_log.write(game);
            if (!dump_dir.empty() && every && game.board().frame() >= dump_from && game.board().frame() % every == 0) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
                if (FILE *d = std::fopen(path, "wb")) {
                    std::fwrite(game.screen().data(), 4, game.screen().size(), d);
                    std::fclose(d);
                }
            }
        }
        if (race_log.is_open()) log_race("end");
        if (solo) throw std::runtime_error("frame budget ended before PC solo race");
        if (!save_nvram_dir.empty()) tools::save_nvram(game, save_nvram_dir);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2run: %" PRIu64 " frames, %" PRIu64 " i960 instructions (all native), %" PRIu64
                    " TGP instructions, %d interrupts, %" PRIu64 " bytes to the sound board; %.2f s (%.0f frames/s)\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.interrupts(),
                    game.board().sound_bytes_total(), s, double(game.frames()) / s);
        std::printf("  last screen hash %016" PRIx64 "\n", game.board().video().screen_hash());
        if (const rt::CommBoard *cb = game.board().comm_board()) {
            static const char *states[] = {"off (the game never started the board)", "waiting for the other cabinets", "up", "lost"};
            std::printf("  link: %s", states[int(cb->link())]);
            if (cb->link() == rt::CommBoard::Link::Up) std::printf(", cabinet %d of %d", cb->id(), cb->count());
            std::printf("\n");
        }
        std::printf("  drive board: %" PRIu64 " commands (", drive_commands);
        for (int k = 0, first = 1; k < 16; k++)
            if (drive_kinds[k]) std::printf("%s%x-: %" PRIu64, first ? "" : ", ", k, drive_kinds[k]), first = 0;
        std::printf(")\n");
        if (const snd::SoundBoard *sb = game.sound())
            std::printf("  sound board: %" PRIu64 " 68000 instructions (all native), %zu command bytes received\n", sb->instructions(),
                        sb->bytes_received());
        if (!wav_path.empty() && game.sound()) {
            std::vector<float> mix;
            mix_into(mix, fm, game.sound()->fm_rate(), 48000);
            mix_into(mix, pcm, game.sound()->pcm_rate(), 48000);
            write_wav(wav_path, mix, 48000);
            std::printf("  wrote %s (%.1f s)\n", wav_path.c_str(), double(mix.size() / 2) / 48000.0);
        }
        if (scene_pair_from) {
            if (!scene_pair[0] || !scene_pair[1]) throw std::runtime_error("incomplete scene pair");
            const auto &first = *scene_pair[0];
            const auto &second = *scene_pair[1];
            const auto a = rt::msc2::read(first.bytes);
            const auto b = rt::msc2::read(second.bytes);
            if (std::printf("scene_pair_verified,%" PRIu64 ",%" PRIu64 ",%zu,%zu\n",
                            first.context.frame, second.context.frame, a.instances.size(), b.instances.size()) < 0 ||
                std::fflush(stdout) != 0)
                throw std::runtime_error("scene pair result write failed");
        }
        if (snapshot_stamps &&
            (game.frames() != frames || std::printf("snapshot_stamp_end,%" PRIu64 "\n", frames) < 0 ||
             std::fflush(stdout) != 0))
            throw std::runtime_error("incomplete snapshot stamp trace");
        if (scene_stamps &&
            (game.frames() != frames || std::printf("scene_stamp_end,%" PRIu64 "\n", frames) < 0 ||
             std::fflush(stdout) != 0))
            throw std::runtime_error("incomplete scene stamp trace");
        if (rom_objects.out.is_open()) rom_objects.out.close();
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2run: stopped: %s\n", e.what());
        return 1;
    }
}
