// m2texcap: the textures the game's polygons use, for the high-resolution
// texture pack (app/gpu/hires_textures.h; scripts/hires_textures.py).
//
//   m2texcap IMAGES_DIR OUT_DIR [--nvram DIR] [--attract FRAMES] [--course N FRAMES]...
//            [--inputs FILE FRAMES]...
//
// Runs the game headless: the attract loop (FRAMES frames, 9000 by default),
// then for each --course (1 Beginner, 2 Advanced, 3 Expert) a solo race on
// it, the throttle held, for FRAMES frames, then for each --inputs a session
// played from power-on by a scripts/inputs file (race_to_end.txt: coins,
// the select screens, a race run out of time, game over and the screens
// after it), for FRAMES frames. Every 20 frames, each textured
// polygon's level-0 texture rectangle is hashed in texture RAM; each new
// texture is written once as OUT_DIR/t_<hash>.pgm (its texels times 17: 0..255)
// and listed in OUT_DIR/textures.csv (sheet, x, y, w, h, hash, cut-out, the
// polygons seen with it). The output is made of the game's textures: it stays
// on this computer (rules.md 6, 22).
#include "app/gpu/hires_textures.h"
#include "runtime/game_loop.h"
#include "runtime/race_request.h"
#include "../common/input_script.h"
#include "../common/nvram.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

struct Seen {
    app::hires::Rect rect;
    bool cutout = false;
    uint64_t polys = 0;
};

void write_pgm(const std::string &path, const uint32_t *sheet, const app::hires::Rect &r) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P5\n%u %u\n255\n", r.w, r.h);
    std::vector<uint8_t> row(r.w);
    for (uint32_t j = 0; j < r.h; ++j) {
        for (uint32_t i = 0; i < r.w; ++i) row[i] = uint8_t(app::hires::texel(sheet, r.x, r.y, int(i), int(j)) * 17);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2texcap IMAGES_DIR OUT_DIR [--nvram DIR] [--attract FRAMES] [--course N FRAMES]... "
                             "[--inputs FILE FRAMES]...\n");
        return 2;
    }
    const std::string dir = argv[1], out = argv[2];
    std::string nvram;
    uint64_t attract = 9000;
    std::vector<std::pair<int, uint64_t>> courses;
    std::vector<std::pair<std::string, uint64_t>> scripts;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--nvram") && i + 1 < argc) nvram = argv[++i];
        else if (!std::strcmp(argv[i], "--attract") && i + 1 < argc) attract = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--course") && i + 2 < argc) {
            const int c = std::atoi(argv[i + 1]);
            courses.emplace_back(c, std::strtoull(argv[i + 2], nullptr, 10));
            i += 2;
        } else if (!std::strcmp(argv[i], "--inputs") && i + 2 < argc) {
            scripts.emplace_back(argv[i + 1], std::strtoull(argv[i + 2], nullptr, 10));
            i += 2;
        }
    }
    std::filesystem::create_directories(out);
    std::map<uint64_t, Seen> seen;
    auto collect = [&](rt::GameLoop &game) {
        const rt::VideoMem &m = game.board().video().gpu_mem();
        if (!m.tex0 || !m.tex1) return;
        for (const rt::GeoPoly &p : game.board().video().gpu_polys()) {
            const int renderer = (p.texheader[0] >> 13) & 3;
            if (renderer < 2 || p.local_texture) continue;
            const app::hires::Rect r = app::hires::rect_of(p.texheader[0], p.texheader[2]);
            if (r.x + r.w > 2048 || r.y + r.h > 1024) continue;
            const uint32_t *sheet = r.sheet ? m.tex1 : m.tex0;
            const uint64_t h = app::hires::hash(sheet, r.x, r.y, r.w, r.h);
            Seen &s = seen[h];
            if (!s.polys) {
                s.rect = r;
                char name[64];
                std::snprintf(name, sizeof name, "t_%016" PRIx64 ".pgm", h);
                write_pgm(out + "/" + name, sheet, r);
            }
            s.cutout |= renderer == 3;
            ++s.polys;
        }
    };
    auto run = [&](rt::GameLoop &game, uint64_t frames, bool throttle) {
        for (uint64_t f = 0; f < frames; ++f) {
            rt::Inputs in{};
            if (throttle && game.board().read_byte(0x5010a0) == 22) in.accel = 0xe0; // racing: the pedal down
            game.run_frame(in);
            if (game.frames() % 20 == 0) collect(game);
        }
    };
    try {
        {
            rt::GameLoop game(dir);
            if (!nvram.empty()) tools::load_nvram(game, nvram);
            game.board().video().set_external_3d(true, true);
            run(game, attract, false);
            std::printf("m2texcap: attract, %" PRIu64 " frames: %zu textures\n", attract, seen.size());
        }
        for (const auto &[course, frames] : courses) {
            rt::GameLoop game(dir);
            if (!nvram.empty()) tools::load_nvram(game, nvram);
            game.board().video().set_external_3d(true, true);
            rt::StartRaceRequest req{};
            req.course = rt::RaceCourse(std::clamp(course, 1, 3) - 1);
            if (game.request_race(req) != rt::RaceStartResult::Accepted) {
                std::fprintf(stderr, "m2texcap: course %d: the race request was refused\n", course);
                continue;
            }
            run(game, frames, true);
            std::printf("m2texcap: course %d, %" PRIu64 " frames: %zu textures in all\n", course, frames, seen.size());
        }
        for (const auto &[path, frames] : scripts) {
            tools::Script script;
            script.load(path);
            rt::GameLoop game(dir);
            if (!nvram.empty()) tools::load_nvram(game, nvram);
            game.board().video().set_external_3d(true, true);
            for (uint64_t f = 0; f < frames; ++f) {
                game.run_frame(script.at(game.board().frame()));
                if (game.frames() % 20 == 0) collect(game);
            }
            std::printf("m2texcap: %s, %" PRIu64 " frames: %zu textures in all\n", path.c_str(), frames, seen.size());
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2texcap: %s\n", e.what());
        return 1;
    }
    std::FILE *csv = std::fopen((out + "/textures.csv").c_str(), "w");
    if (!csv) return 1;
    std::fprintf(csv, "file,sheet,x,y,w,h,hash,cutout,polys\n");
    uint64_t texels = 0;
    for (const auto &[h, s] : seen) {
        std::fprintf(csv, "t_%016" PRIx64 ".pgm,%u,%u,%u,%u,%u,%016" PRIx64 ",%d,%" PRIu64 "\n", h, s.rect.sheet,
                     s.rect.x, s.rect.y, s.rect.w, s.rect.h, h, s.cutout ? 1 : 0, s.polys);
        texels += uint64_t(s.rect.w) * s.rect.h;
    }
    std::fclose(csv);
    std::printf("m2texcap: %zu textures, %" PRIu64 " texels, in %s\n", seen.size(), texels, out.c_str());
    return 0;
}
