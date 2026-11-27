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
//         [--relay HOST:PORT (--relay-create ROOM[:N] [--relay-settings KEY:VALUE,...] | --relay-join ROOM) [--relay-name NAME]
//         [--relay-invite DTN1-... (the game's invite code: server, room and password; joins)]
//          [--relay-password PASS]] (online play through a daytona-relay server)
//         [--realtime] (hold each frame to the arcade's 57.52 Hz, as the game paces itself)
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
#include "app/relay_link.h"
#include "app/selection_steer.h"
#include "runtime/online_settings.h"

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
#include <thread>
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
    int link_place = 0; // --link-place N: LINK ID and CAR NUMBER as the test menu would set them (link play)
    std::string relay_server, relay_create, relay_join, relay_name, relay_password, relay_settings;
    std::optional<rt::StartRaceRequest> solo;
    std::optional<rt::RaceTransmission> transmission;
    std::optional<rt::RaceMode> race_mode;
    bool link_sync = false, native_check = false, realtime = false;
    // M2_TEST_RELAY_SYNC=on: frame sync all along instead of the game's start barrier (comparisons)
    const bool relay_framesync = [] {
        const char *v = std::getenv("M2_TEST_RELAY_SYNC");
        return v && !std::strcmp(v, "on");
    }();
    // M2_TEST_RELAY_PACE=0: zfg flips as MAME, not held for the game's read; =N: paced with N-1 blocks of reserve (comparisons)
    const int relay_pace = [] {
        const char *v = std::getenv("M2_TEST_RELAY_PACE");
        return v && *v ? std::atoi(v) : 1 + rt::online::kLinkReserve; // as the game
    }();
    // M2_TEST_RELAY_PRESET=0: the FF/FE numbering instead of the relay's ring (comparisons)
    const bool relay_preset = [] {
        const char *v = std::getenv("M2_TEST_RELAY_PRESET");
        return !(v && !std::strcmp(v, "0"));
    }();
    // M2_TEST_RELAY_BARRIER=0: no start barrier (comparisons)
    const bool relay_barrier = [] {
        const char *v = std::getenv("M2_TEST_RELAY_BARRIER");
        return !(v && !std::strcmp(v, "0"));
    }();
    bool key_steering = false;
    const bool autodrive = [] {
        const char *v = std::getenv("M2_TEST_AUTODRIVE");
        return v && *v == '1';
    }(); // --key-steering: the course and gearbox picked as the app does with keys (app/selection_steer.h)
    app::SelectionSteer selection_steer;
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
        if (!std::strcmp(argv[i], "--realtime")) realtime = true;
        if (!std::strcmp(argv[i], "--key-steering")) key_steering = true;
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
            !std::strcmp(argv[i], "--realtime") || !std::strcmp(argv[i], "--key-steering") ||
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
        else if (!std::strcmp(argv[i], "--link-place")) link_place = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--link-next")) link_next = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay")) relay_server = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay-create")) relay_create = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay-join")) relay_join = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay-name")) relay_name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay-settings")) relay_settings = argv[i + 1]; // create: key:value,...
        else if (!std::strcmp(argv[i], "--relay-password")) relay_password = argv[i + 1];
        else if (!std::strcmp(argv[i], "--relay-invite")) { // an invite code (DTN1-...): its server, room and password
            const auto invite = app::online::decode_invite(argv[i + 1]);
            if (!invite) {
                std::fprintf(stderr, "m2run: invalid invite code (copy all of it, it starts with DTN1)\n");
                return 1;
            }
            relay_server = invite->server;
            relay_join = invite->room;
            relay_password = invite->password;
        }
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
            if (link_place > 0) rt::online::configure(game.board(), link_place, {});
        }
        // Online: join the room, wait until it is full, then the cabinet's
        // settings from the server and this player's place (as the app does).
        std::unique_ptr<app::RelayLink> relay;
        rt::online::AutoEntry relay_entry;
        rt::online::CourseSync relay_course;
        rt::online::GearboxSync relay_gearbox; // as the app: the MASTER's gearbox for the room (M2_TEST_GEARBOX=manual: the MASTER's)
        const bool relay_manual = [] { const char *v = std::getenv("M2_TEST_GEARBOX"); return v && !std::strcmp(v, "manual"); }();
        rt::online::Standings relay_standings;
        if (!relay_server.empty()) {
            const auto server = app::online::normalize_server(relay_server);
            if (!server) throw std::runtime_error("--relay: bad server address");
            app::online::Hello hello;
            hello.name = relay_name;
            hello.password = relay_password;
            int start_at = 0; // the room's creator starts the race once this many are in (ROOM:N, 2 by default)
            if (!relay_create.empty()) {
                const auto colon = relay_create.rfind(':');
                hello.op = app::online::Hello::Op::Create;
                hello.settings = relay_settings;
                hello.room = relay_create.substr(0, colon);
                start_at = colon == std::string::npos ? 2 : std::atoi(relay_create.c_str() + colon + 1);
                if (start_at < app::online::kMinPlayers || start_at > app::online::kMaxPlayers)
                    throw std::runtime_error("--relay-create ROOM:N needs N from 2 to 8");
            } else {
                hello.room = relay_join;
            }
            relay = std::make_unique<app::RelayLink>(*server, hello);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            bool begun = false;
            while (relay->state() != app::RelayLink::State::Ready) {
                relay->poll();
                if (start_at && !begun && relay->status().owner && relay->status().joined >= start_at) {
                    relay->begin(); // as the room's owner presses Start the race
                    begun = true;
                }
                if (relay->state() == app::RelayLink::State::Failed) throw std::runtime_error("relay: " + relay->error());
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("relay: no race started in 60 s");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto &st = relay->status();
            std::printf("m2run: relay room %s race: player %d of %d (%s)\n", relay->room().c_str(), st.position, st.size,
                        st.position == 1 ? "MASTER" : "SLAVE");
            rt::online::configure(game.board(), st.position, st.eeprom);
            // As the desktop Online mode: no frame sync, the start barrier
            // (rt::online::StartBarrier) lines up the race.
            game.board().set_link(relay.get(), relay_framesync);
            if (relay_preset) game.board().preset_link_ring(rt::online::link_id(st.position, st.size), st.size);
            game.board().set_link_paced(relay_pace > 0, relay_pace > 1 ? relay_pace - 1 : 0,
                                        std::getenv("M2_TEST_RELAY_FLAG_ONLY") != nullptr); // 0.11.0's pacing, for comparisons
            {
                const char *c = std::getenv("M2_PROTO_COALESCE"), *a = std::getenv("M2_PROTO_ADAPTIVE");
                const char *st = std::getenv("M2_PROTO_STEADY"); // =0: 0.12-0.21's adaptive reserve (comparisons)
                // M2_PROTO_RESERVE_MAX=N, M2_PROTO_CALM=N: the steady policy's ceiling and quiet windows (comparisons)
                const char *mx = std::getenv("M2_PROTO_RESERVE_MAX"), *cw = std::getenv("M2_PROTO_CALM");
                game.board().set_link_options(!(c && *c == '0'), !(a && *a == '0'), !(st && *st == '0'),
                                              mx && *mx ? std::atoi(mx) : rt::CommBoard::kSteadyMax,
                                              cw && *cw ? std::atoi(cw) : int(rt::CommBoard::kCalmWindows)); // as the app; 0: off
            }
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
        const auto paced_from = std::chrono::steady_clock::now();
        // Optional online timing evidence: state transitions plus a heartbeat.
        // The monotonic clock can be compared between processes on this host.
        const char *online_trace_path = std::getenv("M2_ONLINE_TRACE");
        std::ofstream online_trace;
        if (online_trace_path && *online_trace_path) {
            online_trace.exceptions(std::ios::failbit | std::ios::badbit);
            online_trace.open(online_trace_path);
            online_trace << "frame,wall_ms,mode,step,course,link,tick\n";
        }
        int previous_mode = -1, previous_step = -1, previous_link = -1;
        bool relay_failure_told = false;
        int races_seen = 0, previous_race_mode = -1;
        // In the race, how far the previous cabinet's tick counter (byte 0x0a of
        // the block received at 0x21c0) moved each frame: 1 is smooth, 0 a frame
        // with no new data, 2+ a skip (the other car would stall or jump).
        uint64_t rx_steps[4] = {};
        int rx_previous = -1;
        // every other car's data, not only the one just ahead in the ring: slot k
        // (k + 1 places behind) as steps 0, 1, 2, 3+ between race frames
        uint64_t car_steps[7][4] = {};
        int car_previous[7] = {-1, -1, -1, -1, -1, -1, -1};
        int car_stall[7] = {}, car_longest[7] = {}; // race frames without new data in a row: now, the longest
        // M2_TEST_AGE_LOG=FILE: in the race, each frame: wall ms (steady), this
        // cabinet's tick counter and the 8 received slots' (comm RAM 0x200a and
        // 0x21c0 + k * 0x1c0 + 0x0a), to work out each other car's data age
        // offline (same-host cabinets share the clock).
        std::FILE *age_log = [] {
            const char *p = std::getenv("M2_TEST_AGE_LOG");
            return p && *p ? std::fopen(p, "w") : nullptr;
        }();
        uint64_t relay_sends_from = 0, relay_messages_from = 0, relay_race_ms = 0, relay_race_from_ms = 0;
        rt::online::StartBarrier relay_barrier_state; // as the app: everyone starts the race together
        bool relay_holding = false;
        auto steady_ms = [] {
            return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
        // Fault injection for the headless timing probe only; never enabled by default.
        const char *delay_env = std::getenv("M2_TEST_FRAME_DELAY_MS");
        const int test_delay_ms = delay_env ? std::max(0, std::atoi(delay_env)) : 0;
        const char *delay_from_env = std::getenv("M2_TEST_FRAME_DELAY_FROM");
        const uint64_t test_delay_from = delay_from_env ? std::strtoull(delay_from_env, nullptr, 10) : 2500;
        // Fault injection: one stall, as the app's Esc pauses the game (M2_TEST_STALL_AT=FRAME, M2_TEST_STALL_MS=MS).
        const char *stall_at_env = std::getenv("M2_TEST_STALL_AT");
        const uint64_t stall_at = stall_at_env ? std::strtoull(stall_at_env, nullptr, 10) : 0;
        const char *stall_ms_env = std::getenv("M2_TEST_STALL_MS");
        const int stall_ms = stall_ms_env ? std::max(0, std::atoi(stall_ms_env)) : 0;
        auto stalled_ns = std::chrono::nanoseconds(0); // the realtime pacing carries on after the stall
        for (uint64_t f = 0; f < frames; f++) {
            if (test_delay_ms && f >= test_delay_from)
                std::this_thread::sleep_for(std::chrono::milliseconds(test_delay_ms));
            if (stall_ms && f == stall_at) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
                stalled_ns += std::chrono::milliseconds(stall_ms);
            }
            if (realtime) // linked cabinets running at the arcade's pace, as players' games do
                std::this_thread::sleep_until(paced_from + stalled_ns + std::chrono::nanoseconds(int64_t(double(f) * 1e9 / 57.524)));
            const bool omit_this_tick = omit_cell && f == omit_cell->first;
            if (omit_this_tick) scenery.omit_cell = omit_cell->second;
            rt::Inputs inputs = script.at(game.board().frame());
            if (autodrive) { // M2_TEST_AUTODRIVE=1: a pedal tap every 100 frames (confirms the screens), held in the race
                const bool racing = game.board().read_byte(0x5010a0) == 22;
                inputs.accel = racing || f % 100 < 8 ? 0xe0 : 0x20;
            }
            if (key_steering) selection_steer.apply(game.board(), inputs.steer, false, relay != nullptr); // as the app with keys
            if (relay) { // as the app: one Start and the MASTER's course for the room
                const auto &room = relay->status();
                const bool master = room.position == 1;
                rt::online::StartGate::apply(game.board(), inputs, master || room.any_start);
                static const bool service_free = std::getenv("M2_TEST_SERVICE_FREE") != nullptr; // diagnostic: 0.15's earlier behaviour
                if (!service_free) rt::online::lock_service(inputs); // as the app: no test menu online
                for (const auto &message : relay->take_controls())
                    if (!relay_barrier_state.receive(message) && !relay_standings.receive(message) && !master &&
                        !relay_gearbox.receive(message))
                        relay_course.receive(message);
                if (master) {
                    if (const auto told = relay_course.master(game.board()); !told.empty()) relay->send_control(told);
                    if (relay_manual && game.board().read_byte(0x5010a0) == rt::online::CourseSync::selection &&
                        game.board().read_byte(0x5010f8) == rt::online::GearboxSync::gearbox_step)
                        inputs.steer = 0xe0; // the MASTER's player chooses manual (the script confirms)
                    if (const auto told = relay_gearbox.master(game.board(), inputs.steer); !told.empty()) relay->send_control(told);
                } else {
                    relay_course.slave(game.board(), inputs);
                    relay_gearbox.slave(game.board(), inputs);
                }
                if (std::getenv("M2_TEST_SELECTION_TRACE")) { // diagnostic: the selection screens' mode and step
                    static int last = -1;
                    const int mode = game.board().read_byte(0x5010a0), step = game.board().read_byte(0x5010f8);
                    if (mode * 256 + step != last && mode >= 15 && mode <= 22)
                        std::printf("m2run: frame %" PRIu64 ": mode %d step %d (steer %02x accel %02x)\n", game.frames(), mode, step,
                                    inputs.steer, inputs.accel);
                    last = mode * 256 + step;
                }
                if (const auto told = relay_standings.own(game.board(), room.position); !told.empty())
                    relay->send_control(told);
                relay_entry.apply(game.board(), inputs);
                if (relay_barrier) {
                    const bool was_holding = relay_holding;
                    static const bool tune = [] {
                        const char *v = std::getenv("M2_PROTO_BARRIER_TUNE");
                        return !(v && *v == '0'); // as the app; 0: off
                    }();
                    relay_holding = relay_barrier_state.holding(room.size, steady_ms(), tune ? &room.latency : nullptr, room.position);
                    if (relay_holding) { // as the app: no game frame until the others arrive, the link still answers
                        const auto frame_ns = std::chrono::nanoseconds(int64_t(1e9 / 57.524));
                        std::this_thread::sleep_for(frame_ns);
                        stalled_ns += frame_ns;
                        game.board().link_idle_tick();
                        --f;
                        continue;
                    }
                    if (was_holding)
                        std::printf("m2run: frame %" PRIu64 ": waited %llu ms for the others at the start\n", game.frames(),
                                    (unsigned long long)relay_barrier_state.waited_ms());
                }
            }
            game.run_frame(inputs);
            if (relay && relay_barrier)
                if (const auto told = relay_barrier_state.after_frame(game.board(), relay->status().position, steady_ms());
                    !told.empty())
                    relay->send_control(told);
            if (relay && relay->state() == app::RelayLink::State::Failed && !relay_failure_told) {
                std::printf("m2run: relay connection failed at frame %" PRIu64 ": %s\n", game.frames(), relay->error().c_str());
                relay_failure_told = true;
            }
            { // each race: its course on entry, the standings this game knew at its end
                const int race_mode = game.board().read_byte(0x5010a0);
                if (relay && race_mode == 22) {
                    const auto now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (previous_race_mode != 22) {
                        relay_sends_from = relay->sends(), relay_messages_from = relay->messages();
                        relay_race_from_ms = now_ms;
                    }
                    relay_race_ms = now_ms - relay_race_from_ms;
                    if (age_log) {
                        const uint8_t *cr = game.board().comm_ram();
                        std::fprintf(age_log, "%llu,%d", (unsigned long long)now_ms, int(cr[0x200a]));
                        for (int k = 0; k < 8; ++k) std::fprintf(age_log, ",%d", int(cr[0x21c0 + k * 0x1c0 + 0x0a]));
                        std::fputc('\n', age_log);
                    }
                    const int rx = game.board().comm_ram()[0x21c0 + 0x0a];
                    if (rx_previous >= 0) ++rx_steps[std::min(3, (rx - rx_previous) & 0xff)];
                    rx_previous = rx;
                    for (int k = 0; k < 7; ++k) {
                        const int tick = game.board().comm_ram()[0x21c0 + k * 0x1c0 + 0x0a];
                        if (car_previous[k] >= 0) {
                            const int step = (tick - car_previous[k]) & 0xff;
                            ++car_steps[k][std::min(3, step)];
                            car_stall[k] = step ? 0 : car_stall[k] + 1;
                            car_longest[k] = std::max(car_longest[k], car_stall[k]);
                        }
                        car_previous[k] = tick;
                    }
                } else {
                    rx_previous = -1;
                    std::fill(std::begin(car_previous), std::end(car_previous), -1);
                }
                if (race_mode == 22 && previous_race_mode != 22)
                    std::printf("m2run: race %d at frame %" PRIu64 ", course %d, gearbox %s\n", ++races_seen, game.frames(),
                                int(game.board().read_byte(0x501460)),
                                game.board().read_byte(0x501464) ? "manual" : "automatic"); // pc_solo.h: the committed gearbox
                if (race_mode != 22 && previous_race_mode == 22 && relay) {
                    std::printf("m2run: race %d ended at frame %" PRIu64 ", standings:", races_seen, game.frames());
                    for (int car = 1; car <= relay->status().size; ++car)
                        std::printf(" %d:%d", car, relay_standings.rank(car) + 1);
                    std::printf("\n");
                }
                previous_race_mode = race_mode;
            }
            if (online_trace.is_open()) {
                const int mode = game.board().read_byte(0x5010a0);
                const int step = game.board().read_byte(0x5010f8);
                const auto *comm = game.board().comm_board();
                const int link_state = comm ? int(comm->link()) : 0;
                if (mode != previous_mode || step != previous_step || link_state != previous_link || f % 60 == 0) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    online_trace << game.frames() << ',' << ms << ',' << mode << ',' << step << ','
                                 << int(game.board().read_byte(0x501460)) << ','
                                 << link_state << ',' << int(game.board().comm_ram()[0x200a]) << '\n';
                    online_trace.flush();
                }
                previous_mode = mode;
                previous_step = step;
                previous_link = link_state;
            }
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
        if (relay) { // the standings this cabinet knew at the end (car:place, 1 = first)
            const uint64_t rx_total = rx_steps[0] + rx_steps[1] + rx_steps[2] + rx_steps[3];
            if (rx_total)
                std::printf("  relay data in the race: %.1f%% steps of 1, %.1f%% no new data, %.1f%% skips (of %" PRIu64 " frames)\n",
                            100.0 * rx_steps[1] / rx_total, 100.0 * rx_steps[0] / rx_total,
                            100.0 * (rx_steps[2] + rx_steps[3]) / rx_total, rx_total);
            // each other car of the room (1, 2, ... places behind in the ring): its steps of 1, the worst too
            if (const int others = relay->status().size - 1; rx_total && others > 1) {
                std::printf("  relay data by car (places behind):");
                double worst = 100.0;
                for (int k = 0; k < others && k < 7; ++k) {
                    const uint64_t n = car_steps[k][0] + car_steps[k][1] + car_steps[k][2] + car_steps[k][3];
                    const double one = n ? 100.0 * car_steps[k][1] / n : 0.0;
                    worst = std::min(worst, one);
                    std::printf(" %d: %.1f%% (%.1f%% none, %.1f%% skips, longest stall %d ms)", k + 1, one,
                                n ? 100.0 * car_steps[k][0] / n : 0.0, n ? 100.0 * (car_steps[k][2] + car_steps[k][3]) / n : 0.0,
                                int(car_longest[k] * 1000.0 / 57.524 + 0.5));
                }
                std::printf("; worst %.1f%%\n", worst);
            }
            if (const rt::CommBoard *cb = game.board().comm_board())
                std::printf("  link reserve: %d at the end, %d changes\n", cb->reserve(), cb->reserve_changes());
            std::printf("  relay UDP: %llu messages delivered first by UDP\n", (unsigned long long)relay->udp_first());
            if (relay_race_ms > 1000)
                std::printf("  relay in the race: %.1f sends/s, %.1f game messages/s\n",
                            (relay->sends() - relay_sends_from) * 1000.0 / relay_race_ms,
                            (relay->messages() - relay_messages_from) * 1000.0 / relay_race_ms);
            std::printf("  relay standings:");
            for (int car = 1; car <= relay->status().size; ++car)
                std::printf(" %d:%d", car, relay_standings.rank(car) + 1);
            std::printf("\n");
        }
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
