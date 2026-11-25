// m2gpushot: the game headless with the hardware renderer, saving frames as
// raw dumps (scripts/rgb2png.py converts them). Same inputs, same frames as
// m2run, whose dumps come from the software renderer: compare the two.
//
//   m2gpushot IMAGES_DIR FRAMES --dump DIR --every N [--inputs scripts/inputs/X.txt]
//             [--aspect W:H [--hud-edges] [--stretch-backdrop]] [--scale N] [--nvram DIR] [--dump-from FRAME]
//             [--draw-distance N] [--draw-budget N] [--scenery-log FILE] [--original-selection]
//             [--msaa N] [--aniso N] [--smooth-cars] [--car-reflections] [--sharp-car-textures] [--smooth-2d] [--car-shadows]
//             [--car-shadows-and-game-shadow] [--bloom] [--crt] [--window WxH] [--pixels WxH]
//             [--hires-textures PACK]
//   m2gpushot - 0 --quality-probe WxH: the automatic quality's measurement
//             (app/gpu/quality_probe.h) for a WxH screen; no game is run
//   m2gpushot IMAGES_DIR FRAMES --bench [--inputs ...] [--aspect ...] [--scale N]
//
// --scale N: the internal resolution enhancement (1-4); dumps are N times
// wider and taller. --nvram DIR: start from the game's saved settings EEPROM
// and backup RAM (ioboard_eeprom.bin, backup_ram.bin, as the app saves them
// in its data folder), e.g. a cabinet type set in test mode.
//
// --bloom, --crt: the display passes (GpuRenderer::present); the dumps are
// then the window, --window WxH (1920x1080), the frame letterboxed in it.
//
// --bench draws every frame through the GPU, with no readback, and reports
// the time in the game (logic, geometrizer, CPU tilemap layers), in the
// renderer on the CPU (vertices, uploads) and waiting for the GPU.
//
// SDL_GPU without a window: an offscreen texture is drawn and read back.

#include "app/gpu/gpu_renderer.h"
#include "app/gpu/quality_probe.h"
#include "runtime/game_loop.h"
#include "../common/input_script.h"
#include "../common/nvram.h"
#include "../common/scenery_log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <vector>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2gpushot IMAGES_DIR FRAMES --dump DIR --every N [--inputs FILE]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path, nvram_dir, scenery_log_path;
    uint32_t draw_budget = 0;
    uint64_t every = 0, dump_from = 0;
    int scale = 1;
    std::string hires_path; // --hires-textures: a high-resolution texture pack (app/gpu/hires_textures.h)
    int pixels_w = 0, pixels_h = 0; // --pixels WxH: the 3D drawn at that size, as the native mode does
    int window_w = 1920, window_h = 1080; // --bloom, --crt: the window the frame is shown in
    bool bloom = false, crt = false;
    int msaa = 1, aniso = 1; // the MSAA and texture filtering enhancements (GpuRenderer::init, set_anisotropy)
    bool smooth_cars = false, car_reflections = false; // GpuRenderer::set_car_shading
    double aspect = 0;
    bool hud_edges = false, stretch = false, bench = false, original_selection = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--draw-budget") && i + 1 == argc) {
            std::fprintf(stderr, "m2gpushot: --draw-budget needs a value\n");
            return 2;
        }
        if (!std::strcmp(argv[i], "--bench")) bench = true;
        if (!std::strcmp(argv[i], "--hud-edges")) hud_edges = true;
        if (!std::strcmp(argv[i], "--stretch-backdrop")) stretch = true;
        if (!std::strcmp(argv[i], "--original-selection")) original_selection = true;
    }
    // options with a value take the next argument; switches none (read in
    // the loops before and after); anything else is skipped on its own, so an
    // option's place on the command line never changes what is read
    auto is_switch = [](const char *a) {
        for (const char *s : {"--hud-edges", "--stretch-backdrop", "--bench", "--original-selection", "--smooth-cars",
                              "--car-reflections", "--sharp-car-textures", "--smooth-2d", "--car-shadows",
                              "--car-shadows-and-game-shadow", "--bloom", "--crt"})
            if (!std::strcmp(a, s)) return true;
        return false;
    };
    for (int i = 3; i + 1 < argc; ++i) {
        if (is_switch(argv[i]) || std::strncmp(argv[i], "--", 2) != 0) continue;
        if (!std::strcmp(argv[i], "--aspect")) {
            double a = 0, b = 0;
            if (std::sscanf(argv[i + 1], "%lf:%lf", &a, &b) == 2 && b > 0) aspect = a / b;
        }
        if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--dump-from")) dump_from = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--scale")) scale = std::clamp(std::atoi(argv[i + 1]), 1, 4);
        else if (!std::strcmp(argv[i], "--msaa")) msaa = std::clamp(std::atoi(argv[i + 1]), 1, 8);
        else if (!std::strcmp(argv[i], "--aniso")) aniso = std::clamp(std::atoi(argv[i + 1]), 1, 16);
        else if (!std::strcmp(argv[i], "--window")) std::sscanf(argv[i + 1], "%dx%d", &window_w, &window_h);
        else if (!std::strcmp(argv[i], "--pixels")) std::sscanf(argv[i + 1], "%dx%d", &pixels_w, &pixels_h);
        else if (!std::strcmp(argv[i], "--hires-textures")) hires_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-distance")) rt::GameLoop::set_draw_distance(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--scenery-log")) scenery_log_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-budget")) {
            if (!rt::parse_scenery_budget(argv[i + 1], draw_budget)) {
                std::fprintf(stderr, "m2gpushot: --draw-budget needs 0 (Automatic) or 1..1000000\n");
                return 2;
            }
        }
    }
    bool probe_only = false;
    for (int i = 3; i < argc; ++i) probe_only |= !std::strcmp(argv[i], "--quality-probe");
    if (!bench && !probe_only && (dump_dir.empty() || !every)) {
        std::fprintf(stderr, "m2gpushot: --dump DIR and --every N (or --bench) are needed\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "m2gpushot: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                             false, nullptr);
    if (!dev) {
        std::fprintf(stderr, "m2gpushot: SDL_CreateGPUDevice: %s\n", SDL_GetError());
        return 1;
    }
    std::printf("m2gpushot: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));
    for (int i = 3; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--quality-probe")) {
            int pw = 1920, ph = 1080;
            std::sscanf(argv[i + 1], "%dx%d", &pw, &ph);
            app::QualityProbe probe;
            const char *driver = SDL_GetGPUDeviceDriver(dev);
            probe.start(dev, pw, ph, !std::strcmp(driver, "vulkan") || !std::strcmp(driver, "direct3d12"));
            const auto t0 = SDL_GetTicks();
            while (probe.running()) probe.step();
            const auto &t = probe.times();
            static const char *names[] = {"original", "medium", "high", "maximum"};
            const auto mx = probe.size_of(app::QualityProbe::Stage::Maximum);
            std::printf("m2gpushot: %s; maximum (%dx%d) %.2f ms, high %.2f ms, medium %.2f ms a frame; level %s; "
                        "%llu ms%s%s\n",
                        probe.gpu_name().c_str(), mx.w, mx.h, t.maximum, t.high, t.medium, names[int(probe.level())],
                        static_cast<unsigned long long>(SDL_GetTicks() - t0),
                        probe.stage() == app::QualityProbe::Stage::Failed ? "; failed: " : "", probe.error().c_str());
            SDL_DestroyGPUDevice(dev);
            return probe.stage() == app::QualityProbe::Stage::Failed;
        }
    app::GpuRenderer gpu;
    if (!gpu.init(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, msaa)) {
        std::fprintf(stderr, "m2gpushot: %s\n", gpu.error().c_str());
        return 1;
    }
    gpu.set_anisotropy(aniso);
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--smooth-cars")) smooth_cars = true;
        if (!std::strcmp(argv[i], "--car-reflections")) car_reflections = true;
        if (!std::strcmp(argv[i], "--sharp-car-textures")) gpu.set_car_textures(true);
        if (!std::strcmp(argv[i], "--smooth-2d")) gpu.set_smooth_2d(true);
        if (!std::strcmp(argv[i], "--car-shadows")) gpu.set_car_shadows(true, true);
        if (!std::strcmp(argv[i], "--car-shadows-and-game-shadow")) gpu.set_car_shadows(true, false);
        if (!std::strcmp(argv[i], "--bloom")) bloom = true;
        if (!std::strcmp(argv[i], "--crt")) crt = true;
    }
    const bool post = bloom || crt;
    gpu.set_car_shading(smooth_cars, car_reflections);
    if (!hires_path.empty() && !gpu.load_hires_textures(hires_path)) {
        std::fprintf(stderr, "m2gpushot: %s\n", gpu.error().c_str());
        return 1;
    }
    if (msaa > 1 || aniso > 1) std::printf("m2gpushot: MSAA %dx, anisotropic %dx\n", gpu.msaa(), aniso);
    constexpr int H = rt::GameLoop::kHeight;
    const int W = rt::GameLoop::kWidth + 2 * rt::GameLoop::wide_margin(aspect);
    const int SW = pixels_w > 0 ? pixels_w : W * scale, SH = pixels_w > 0 ? pixels_h : H * scale; // the target
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(SW);
    ti.height = Uint32(SH);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *target = SDL_CreateGPUTexture(dev, &ti);
    const int OW = post ? window_w : SW, OH = post ? window_h : SH; // the dumps
    SDL_GPUTexture *window = nullptr;
    if (post) {
        ti.width = Uint32(OW), ti.height = Uint32(OH);
        window = SDL_CreateGPUTexture(dev, &ti);
    }
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = Uint32(OW) * Uint32(OH) * 4;
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!target || !download) {
        std::fprintf(stderr, "m2gpushot: %s\n", SDL_GetError());
        return 1;
    }

    try {
        rt::GameLoop game(dir);
        game.set_draw_budget(draw_budget);
        game.board().scenery()->original_selection = original_selection;
        tools::SceneryLog scenery_log(scenery_log_path);
        scenery_log.attach(game);
        if (!nvram_dir.empty()) tools::load_nvram(game, nvram_dir);
        game.board().video().set_external_3d(true, true);
        game.set_aspect(aspect);
        game.set_hud_edges(hud_edges);
        game.set_stretch_backdrop(stretch);
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        if (bench) {
            using clk = std::chrono::steady_clock;
            double t_game = 0, t_cpu = 0, t_wait = 0;
            // the CPU tilemap work inside the game's frame (Video's own timers, nanoseconds)
            game.board().video().set_profile_clock([]() -> uint64_t {
                return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count());
            });
            double t_tile_cache = 0, t_tile_draw = 0, t_composite = 0;
            std::vector<double> frame_ms; // the game's frame and the renderer's CPU work, each frame
            frame_ms.reserve(size_t(frames));
            SDL_GPUFence *last = nullptr;
            const auto t0 = clk::now();
            for (uint64_t f = 0; f < frames; f++) {
                const auto a = clk::now();
                game.run_frame(script.at(game.board().frame()));
                const auto b = clk::now();
                if (game.board().frame() >= dump_from)
                    scenery_log.write(game);
                const rt::VideoProfile &vp = game.board().video().last_profile();
                t_tile_cache += double(vp.tile_cache) * 1e-9;
                t_tile_draw += double(vp.tile_draw) * 1e-9;
                t_composite += double(vp.composite) * 1e-9;
                if (last) { // one frame in flight, as a window would have
                    SDL_WaitForGPUFences(dev, true, &last, 1);
                    SDL_ReleaseGPUFence(dev, last);
                }
                const auto c = clk::now();
                SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
                gpu.set_light(game.board().light());
                gpu.render(cmd, target, W, H, game.board().video(), scale, pixels_w > 0 ? SW : 0, pixels_w > 0 ? SH : 0);
                last = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                const auto d = clk::now();
                t_game += std::chrono::duration<double>(b - a).count();
                t_wait += std::chrono::duration<double>(c - b).count();
                t_cpu += std::chrono::duration<double>(d - c).count();
                frame_ms.push_back(std::chrono::duration<double>((b - a) + (d - c)).count() * 1e3);
            }
            if (last) SDL_WaitForGPUFences(dev, true, &last, 1), SDL_ReleaseGPUFence(dev, last);
            const double total = std::chrono::duration<double>(clk::now() - t0).count();
            std::printf("m2gpushot: %" PRIu64 " frames in %.2f s (%.0f frames/s); per frame: game %.2f ms, "
                        "renderer CPU %.2f ms, waiting for the GPU %.2f ms\n",
                        frames, total, double(frames) / total, t_game * 1e3 / double(frames),
                        t_cpu * 1e3 / double(frames), t_wait * 1e3 / double(frames));
            if (!frame_ms.empty()) { // the slow frames: the game's and the renderer's CPU time
                std::vector<double> sorted = frame_ms;
                std::sort(sorted.begin(), sorted.end());
                auto at = [&](double q) { return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size())))]; };
                size_t over = 0;
                for (double t : frame_ms) over += t > 1000.0 / 57.52 * 0.5;
                std::printf("m2gpushot: game + renderer CPU per frame: median %.2f ms, p95 %.2f ms, p99 %.2f ms, "
                            "max %.2f ms; %zu frames over half a frame (8.7 ms)\n",
                            at(0.5), at(0.95), at(0.99), sorted.back(), over);
            }
            std::printf("m2gpushot: of the game's time, CPU tilemaps per frame: decoding layers %.2f ms, "
                        "drawing them %.2f ms, composing %.2f ms\n",
                        t_tile_cache * 1e3 / double(frames), t_tile_draw * 1e3 / double(frames),
                        t_composite * 1e3 / double(frames));
            gpu.shutdown();
            SDL_DestroyGPUDevice(dev);
            return 0;
        }
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            if (game.board().frame() >= dump_from) scenery_log.write(game);
            if (game.board().frame() < dump_from || game.board().frame() % every) continue;
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
            gpu.set_light(game.board().light());
            gpu.render(cmd, target, W, H, game.board().video(), scale, pixels_w > 0 ? SW : 0, pixels_w > 0 ? SH : 0);
            if (post) {
                app::GpuRenderer::PostFrame pf;
                pf.source = target;
                pf.target = window;
                const double fit = std::min(double(OW) / W, double(OH) / H);
                const int dw = int(W * fit), dh = int(H * fit);
                pf.dest = SDL_Rect{(OW - dw) / 2, (OH - dh) / 2, dw, dh};
                pf.lines = H;
                pf.bloom = bloom ? 0.35f : 0.0f;
                pf.crt = crt ? 1.0f : 0.0f;
                if (!gpu.present(cmd, pf)) std::fprintf(stderr, "m2gpushot: present failed\n");
            }
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = post ? window : target;
            src.w = Uint32(OW);
            src.h = Uint32(OH);
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = download;
            dst.pixels_per_row = Uint32(OW);
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            char path[512];
            std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
            if (FILE *d = std::fopen(path, "wb")) {
                const void *p = SDL_MapGPUTransferBuffer(dev, download, false);
                std::fwrite(p, 4, size_t(OW) * size_t(OH), d);
                SDL_UnmapGPUTransferBuffer(dev, download);
                std::fclose(d);
            }
        }
        std::printf("m2gpushot: %" PRIu64 " frames\n", frames);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2gpushot: %s\n", e.what());
        return 1;
    }
    gpu.shutdown();
    SDL_ReleaseGPUTransferBuffer(dev, download);
    SDL_ReleaseGPUTexture(dev, target);
    if (window) SDL_ReleaseGPUTexture(dev, window);
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    return 0;
}
