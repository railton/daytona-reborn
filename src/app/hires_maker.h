// The high-resolution texture pack maker (Settings > AI textures): makes
// the pack (app/gpu/hires_textures.h, docs/HIRES-TEXTURES.md) on this
// computer, from the player's own ROM set, in the background:
//
//   1. capture: tools/m2texcap (built with the game, next to it) plays the
//      attract loop, a race on each course and a session through the select
//      screens, and writes the textures it saw;
//   2. tools: Linux: Docker builds the upscaling container (hires/Dockerfile,
//      PyTorch with CUDA); it needs Docker usable by this user and, for the
//      graphics card, the NVIDIA Container Toolkit;
//   3. upscale: hires/hires_textures.py in the container downloads the models
//      (SHA-256 checked), upscales each texture by the model for its kind and
//      writes the pack;
//   4. install: the pack, checked, to <pref dir>/hires.m2tex;
//   5. clean up, after a success only: the captured textures, the models,
//      the container image and its base image (unless it was there before).
//
// The kit (hires/: the script, the textures' kinds, the Dockerfile, the
// capture's session) is copied next to the game by the build. Every command
// and its output go to <pref dir>/hires-textures.log, which a failure names.
// The graphics card: an NVIDIA card with its driver (nvidia-smi answers);
// without one the processor can do it, much slower (about 30 minutes on 20
// threads, estimated from a 3 % sample). Windows and macOS: not yet.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

class HiresMaker {
public:
    enum class Stage { Idle, Checking, Capturing, Building, Upscaling, Installing, Cleaning, Done, Failed, Cancelled };
    // Why it failed (the launcher has each one's text; the log the details).
    enum class Problem { None, Unsupported, Rom, Kit, DockerMissing, DockerAccess, NoNvidia, NoToolkit,
                         Capture, Build, Upscale, BadPack, Save };

    // What this computer has for it (check()).
    struct Requirements {
        bool checked = false;
        bool supported = false;   // this system has a way (Linux, for now)
        bool nvidia = false;      // an NVIDIA card answering nvidia-smi
        std::string gpu;          // its name
        bool docker = false;      // Docker installed and usable by this user
        bool docker_installed = false;
        bool docker_gpu = false;  // the NVIDIA Container Toolkit (Docker's nvidia runtime or its hook)
        bool kit = false;         // hires/ and m2texcap next to the game
    };

    struct State {
        Stage stage = Stage::Idle;
        float progress = 0;       // 0..1, the whole job
        Problem problem = Problem::None; // Failed: what went wrong (the log has the rest)
        int exit_code = 0;               // the failing program's exit code, if one failed
        Requirements req;
        uint64_t pack_version = 0; // raised each time a pack is installed or deleted
    };

    HiresMaker();
    ~HiresMaker(); // cancels and waits

    void check();                                            // the requirements, in the background
    bool start(const std::string &rom_path, bool processor); // the job, in the background
    void cancel();
    bool busy() const;
    State state() const;
    bool delete_pack();

    static std::string pack_path(); // <pref dir>/hires.m2tex
    static std::string log_path();  // <pref dir>/hires-textures.log

    // "TOOL: progress DONE/TOTAL" (m2texcap, hires_textures.py): true with
    // the numbers. Exposed for the tests.
    static bool parse_progress(const std::string &line, const std::string &tool, uint64_t &done, uint64_t &total);
    // the base image a Dockerfile starts FROM ("" if none). For the tests.
    static std::string dockerfile_base(const std::string &text);

private:
    void join();
    void set_stage(Stage s, float progress);
    void set_progress(float progress);
    void fail(Problem problem, const std::string &english, int exit_code = 0);
    void log(const std::string &line);
    // Runs a program, each line of its output (stderr too) to the log and
    // on_line; the exit code, or -1 if it cannot start or was cancelled.
    int run(const std::vector<std::string> &args, const std::function<void(const std::string &)> &on_line = {});
    Requirements probe();
    void job(std::string rom_path, bool processor);

    mutable std::mutex mutex_;
    State state_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, busy_{false};
    std::mutex log_mutex_;
};

} // namespace app
