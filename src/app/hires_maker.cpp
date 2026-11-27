#include "app/hires_maker.h"

#include "app/config.h"
#include "app/custom_cars.h"
#include "app/gpu/hires_textures.h"
#include "app_version.h" // generated: cmake/app_version.cmake
#include "runtime/archive.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(SDL_PLATFORM_LINUX)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace app {

namespace {

constexpr const char *kImage = "daytona-hires-kit"; // the upscaling container, removed after a success
// its run's name: killing `docker run` leaves the container running (it went
// on upscaling after a cancel, into the folder the next run uses), so a
// cancel removes it by this name, and a run removes one left before
constexpr const char *kContainer = "daytona-hires-kit-run";

std::string base_dir() {
    const char *base = SDL_GetBasePath();
    return base ? base : "";
}
std::string kit_dir() { return base_dir() + "hires/"; }
std::string capture_tool() {
#if defined(SDL_PLATFORM_WINDOWS)
    return base_dir() + "m2texcap.exe";
#else
    return base_dir() + "m2texcap";
#endif
}
std::string work_dir() { return Config::pref_dir() + "hires-work/"; }

// `docker rm -f kContainer` (kills it if running), outside run(): it runs after a cancel.
void remove_container(const std::function<void(const std::string &)> &log) {
    const char *argv[] = {"docker", "rm", "-f", kContainer, nullptr};
    log(std::string("$ docker rm -f ") + kContainer);
    SDL_Process *p = SDL_CreateProcess(argv, false);
    if (!p) return log(std::string("cannot start: ") + SDL_GetError());
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    log("exit " + std::to_string(code));
}

std::string now_text() {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

bool exists(const std::string &path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

} // namespace

// The process being run, for cancel() (one at a time: the worker's).
static std::mutex g_process_mutex;
static SDL_Process *g_process = nullptr;

HiresMaker::HiresMaker() = default;

HiresMaker::~HiresMaker() {
    cancel();
    join();
}

void HiresMaker::join() {
    if (worker_.joinable()) worker_.join();
}

std::string HiresMaker::textures_dir() {
    const std::string base = base_dir(), image = beside_appimage();
    for (const std::string &dir : {image.empty() ? std::string() : image + "textures/", base + "../textures/", std::string("textures/")})
        if (std::error_code ec; !dir.empty() && fs::is_directory(fs::u8path(dir), ec)) return dir;
    // none yet: beside the .AppImage; the project's (beside roms/, as the build finds the ROM set); else the working folder's
    if (!image.empty()) return image + "textures/";
    if (std::error_code ec; fs::is_directory(fs::u8path(base + "../roms/"), ec)) return base + "../textures/";
    return "textures/";
}

std::string HiresMaker::pack_path() { return textures_dir() + "hires.m2tex"; }

std::string HiresMaker::prepare_pack(std::string &message, const std::string &dir_in, const std::string &old_in) {
    const std::string dir = dir_in.empty() ? textures_dir() : dir_in;
    const std::string pack = dir + "hires.m2tex";
    const std::string old_pack = old_in.empty() ? Config::pref_dir() + "hires.m2tex" : old_in;
    std::error_code ec;
    message.clear();
    auto valid = [](const std::string &path, std::string &why) {
        hires::Pack p;
        if (!p.load(path)) return why = p.error, false;
        if (p.scale != 4) return why = "scale " + std::to_string(p.scale) + ", 4 expected", false;
        return true;
    };
    auto note = [&](const std::string &line) { message += (message.empty() ? "" : "; ") + line; };
    std::string why;
    if (fs::exists(fs::u8path(pack), ec)) {
        if (valid(pack, why)) return pack;
        note("textures/hires.m2tex left out: " + why);
    }
    // a .zip (or .7z) holding a pack: the first one in name order that is valid
    std::vector<fs::path> archives;
    for (const auto &e : fs::directory_iterator(fs::u8path(dir), ec)) {
        std::string ext = e.path().extension().string();
        for (char &c : ext) c = char(std::tolower((unsigned char)c));
        if (e.is_regular_file(ec) && (ext == ".zip" || ext == ".7z")) archives.push_back(e.path());
    }
    std::sort(archives.begin(), archives.end());
    for (const fs::path &a : archives) {
        const auto u8 = a.filename().u8string();
        const std::string name(u8.begin(), u8.end());
        try {
            const auto archive = rt::open_archive(a.string());
            std::string member;
            for (const auto &[entry, info] : archive->entries())
                if (entry.size() > 6 && entry.compare(entry.size() - 6, 6, ".m2tex") == 0) { member = entry; break; }
            if (member.empty()) { note("textures/" + name + " left out: no .m2tex in it"); continue; }
            const std::vector<uint8_t> bytes = archive->read(member);
            const std::string part = pack + ".part";
            {
                std::ofstream out(fs::u8path(part), std::ios::binary);
                out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
                if (!out) { note("textures/" + name + ": hires.m2tex could not be written"); continue; }
            }
            if (!valid(part, why)) {
                fs::remove(fs::u8path(part), ec);
                note("textures/" + name + " left out: " + why);
                continue;
            }
            fs::remove(fs::u8path(pack), ec);
            fs::rename(fs::u8path(part), fs::u8path(pack), ec);
            if (ec) { note("textures/" + name + ": " + ec.message()); continue; }
            note("the pack unpacked from textures/" + name);
            return pack;
        } catch (const std::exception &e) {
            note("textures/" + name + " left out: " + e.what());
        }
    }
    // the pack where it was kept before (the data folder): moved to textures/
    if (fs::exists(fs::u8path(old_pack), ec) && valid(old_pack, why)) {
        fs::create_directories(fs::u8path(dir), ec);
        fs::rename(fs::u8path(old_pack), fs::u8path(pack), ec);
        if (ec) { // another drive: copy, then remove
            ec.clear();
            fs::copy_file(fs::u8path(old_pack), fs::u8path(pack), fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::remove(fs::u8path(old_pack), ec);
        }
        if (!ec) { note("the pack moved from " + old_pack); return pack; }
    }
    return {};
}
std::string HiresMaker::log_path() { return Config::logs_dir() + "hires-textures.log"; }

bool HiresMaker::parse_progress(const std::string &line, const std::string &tool, uint64_t &done, uint64_t &total) {
    const std::string key = tool + ": progress ";
    const size_t at = line.find(key);
    if (at == std::string::npos) return false;
    unsigned long long d = 0, t = 0;
    if (std::sscanf(line.c_str() + at + key.size(), "%llu/%llu", &d, &t) != 2 || t == 0 || d > t) return false;
    done = d, total = t;
    return true;
}

std::string HiresMaker::dockerfile_base(const std::string &text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line.compare(s, 5, "FROM ") != 0) continue;
        std::istringstream words(line.substr(s + 5));
        std::string image;
        words >> image;
        return image;
    }
    return "";
}

bool HiresMaker::busy() const { return busy_; }

HiresMaker::State HiresMaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void HiresMaker::set_stage(Stage s, float progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.stage = s;
    state_.progress = progress;
}

void HiresMaker::set_progress(float progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.progress = progress;
}

void HiresMaker::fail(Problem problem, const std::string &english, int exit_code) {
    log(cancel_ ? "cancelled" : "FAILED: " + english + (exit_code ? " (exit " + std::to_string(exit_code) + ")" : ""));
    std::lock_guard<std::mutex> lock(mutex_);
    state_.stage = cancel_ ? Stage::Cancelled : Stage::Failed;
    state_.problem = problem;
    state_.exit_code = exit_code;
}

void HiresMaker::log(const std::string &line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::ofstream f(log_path(), std::ios::app);
    f << "[" << now_text() << "] " << line << "\n";
}

int HiresMaker::run(const std::vector<std::string> &args, const std::function<void(const std::string &)> &on_line) {
    std::string command;
    for (const std::string &a : args) command += (command.empty() ? "" : " ") + a;
    log("$ " + command);
    if (cancel_) return -1;
    std::vector<const char *> argv;
    for (const std::string &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        log(std::string("cannot start: ") + SDL_GetError());
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_process = process;
    }
    std::string pending;
    auto emit = [&](const std::string &line) {
        if (line.empty()) return;
        log("  " + line);
        if (on_line) on_line(line);
    };
    if (SDL_IOStream *out = SDL_GetProcessOutput(process)) {
        char buf[4096];
        for (;;) {
            const size_t n = SDL_ReadIO(out, buf, sizeof buf);
            if (n == 0) {
                if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                    SDL_Delay(10);
                    continue;
                }
                break; // the end, or an error
            }
            for (size_t i = 0; i < n; ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') emit(pending), pending.clear();
                else pending += buf[i];
            }
        }
    }
    emit(pending);
    int code = -1;
    SDL_WaitProcess(process, true, &code);
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_process = nullptr;
    }
    SDL_DestroyProcess(process);
    log("exit " + std::to_string(code));
    return cancel_ ? -1 : code;
}

void HiresMaker::cancel() {
    cancel_ = true;
    std::lock_guard<std::mutex> lock(g_process_mutex);
    if (g_process) SDL_KillProcess(g_process, true);
}

HiresMaker::Requirements HiresMaker::probe() {
    Requirements r;
    r.checked = true;
#if defined(SDL_PLATFORM_LINUX)
    r.supported = true;
#endif
    r.kit = exists(kit_dir() + "hires_textures.py") && exists(kit_dir() + "Dockerfile") && exists(capture_tool());
    std::string first;
    const int smi = run({"nvidia-smi", "--query-gpu=name", "--format=csv,noheader"},
                        [&](const std::string &line) { if (first.empty()) first = line; });
    r.nvidia = smi == 0 && !first.empty();
    if (r.nvidia) r.gpu = first;
    if (!r.supported) return r;
    const int version = run({"docker", "version", "--format", "{{.Server.Version}}"});
    r.docker = version == 0;
    r.docker_installed = version != -1 && version != 127;
    if (r.docker) {
        std::string runtimes;
        run({"docker", "info", "--format", "{{range $k, $v := .Runtimes}}{{$k}} {{end}}"},
            [&](const std::string &line) { runtimes += line; });
        r.docker_gpu = runtimes.find("nvidia") != std::string::npos ||
                       exists("/usr/bin/nvidia-container-runtime-hook") || exists("/usr/bin/nvidia-container-toolkit") ||
                       exists("/etc/cdi/nvidia.yaml") || exists("/var/run/cdi/nvidia.yaml");
    }
    return r;
}

void HiresMaker::check() {
    if (busy_) return;
    join();
    busy_ = true;
    cancel_ = false;
    set_stage(Stage::Checking, 0);
    worker_ = std::thread([this] {
        const Requirements r = probe();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.req = r;
            state_.stage = Stage::Idle;
        }
        busy_ = false;
    });
}

bool HiresMaker::start(const std::string &rom_path, bool processor) {
    if (busy_) return false;
    join();
    busy_ = true;
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.problem = Problem::None;
        state_.exit_code = 0;
        state_.stage = Stage::Capturing;
        state_.progress = 0;
    }
    worker_ = std::thread([this, rom_path, processor] {
        job(rom_path, processor);
        busy_ = false;
    });
    return true;
}

bool HiresMaker::delete_pack() {
    if (busy_) return false;
    std::error_code ec;
    fs::remove(pack_path(), ec);
    std::lock_guard<std::mutex> lock(mutex_);
    ++state_.pack_version;
    return !ec;
}

void HiresMaker::job(std::string rom_path, bool processor) {
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream(log_path(), std::ios::trunc) << "Daytona " << DAYTONA_APP_VERSION << " (" << DAYTONA_APP_COMMIT
                                                    << "): high-resolution texture pack, " << now_text() << "\n";
    }
    log(std::string("ROM set: ") + rom_path + (processor ? "; on the processor" : "; on the graphics card"));
    const Requirements req = probe();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.req = req;
    }
    if (!req.supported) return fail(Problem::Unsupported, "this system cannot make the pack yet (Linux only)");
    if (rom_path.empty() || !exists(rom_path)) return fail(Problem::Rom, "no ROM set at '" + rom_path + "'");
    if (!req.kit) return fail(Problem::Kit, "the maker's files are missing next to the game (hires/, m2texcap)");
    if (!req.docker)
        return req.docker_installed ? fail(Problem::DockerAccess, "Docker is installed but this user cannot use it")
                                    : fail(Problem::DockerMissing, "Docker is not installed");
    if (!processor && !req.nvidia) return fail(Problem::NoNvidia, "no NVIDIA graphics card answered (nvidia-smi)");
    if (!processor && !req.docker_gpu)
        return fail(Problem::NoToolkit, "Docker cannot use the graphics card: no NVIDIA Container Toolkit");

    const std::string work = work_dir(), kit = kit_dir();
    std::error_code ec;
    // a run stopped before (a failure, a cancel) leaves its upscaled images
    // (capture/out, with how each was made): kept, hires_textures.py takes
    // up the ones done; the capture itself is made again
    std::vector<fs::path> old;
    for (const auto &e : fs::directory_iterator(fs::u8path(work + "capture"), ec))
        if (e.path().filename() != "out") old.push_back(e.path());
    for (const fs::path &p : old) fs::remove_all(p, ec);
    ec.clear();
    fs::create_directories(work + "capture", ec);
    fs::create_directories(work + "models", ec);
    fs::remove(work + "hires.m2tex", ec);

    // 1. capture
    set_stage(Stage::Capturing, 0);
    uint64_t done = 0, total = 0;
    int code = run({capture_tool(), rom_path, work + "capture", "--attract", "9000", "--course", "1", "4000", "--course",
                    "2", "4000", "--course", "3", "4000", "--inputs", kit + "race_to_end.txt", "20000"},
                   [&](const std::string &line) {
                       if (parse_progress(line, "m2texcap", done, total)) set_progress(0.25f * float(done) / float(total));
                   });
    if (code != 0) return fail(Problem::Capture, "the capture of the game's textures failed", code);

    // 2. the container
    set_stage(Stage::Building, 0.25f);
    std::string dockerfile;
    {
        std::ifstream f(kit + "Dockerfile");
        std::ostringstream s;
        s << f.rdbuf();
        dockerfile = s.str();
    }
    const std::string base = dockerfile_base(dockerfile);
    const bool base_there = !base.empty() && run({"docker", "image", "inspect", "--format", "{{.Id}}", base}) == 0;
    int lines = 0;
    code = run({"docker", "build", "--progress", "plain", "-t", kImage, kit},
               [&](const std::string &) { set_progress(std::min(0.39f, 0.25f + 0.0005f * float(++lines))); });
    if (code != 0) return fail(Problem::Build, "Docker could not build the upscaling container", code);

    // 3. upscale
    set_stage(Stage::Upscaling, 0.4f);
    remove_container([this](const std::string &line) { log(line); });
    std::vector<std::string> args = {"docker", "run", "--rm", "--name", kContainer};
    if (!processor) args.insert(args.end(), {"--gpus", "all"});
#if defined(SDL_PLATFORM_LINUX)
    args.insert(args.end(), {"-u", std::to_string(getuid()) + ":" + std::to_string(getgid())});
#endif
    args.insert(args.end(), {"-e", "HOME=/tmp", "-v", kit + ":/hires:ro", "-v", work + ":/work", kImage, "python3", "-u",
                             "/hires/hires_textures.py", "all", "/work/capture", "/work/models", "/work/hires.m2tex"});
    code = run(args, [&](const std::string &line) {
        if (parse_progress(line, "hires_textures", done, total)) set_progress(0.4f + 0.55f * float(done) / float(total));
    });
    if (cancel_) remove_container([this](const std::string &line) { log(line); });
    if (code != 0) return fail(Problem::Upscale, "the upscaling failed", code);

    // 4. install
    set_stage(Stage::Installing, 0.95f);
    fs::create_directories(fs::u8path(textures_dir()), ec);
    hires::Pack pack;
    if (!pack.load(work + "hires.m2tex")) return fail(Problem::BadPack, "the pack made is not valid: " + pack.error);
    log("pack: " + std::to_string(pack.entries.size()) + " textures, " + std::to_string(pack.data.size()) + " bytes");
    fs::remove(pack_path(), ec);
    fs::rename(work + "hires.m2tex", pack_path(), ec);
    if (ec) {
        ec.clear();
        fs::copy_file(work + "hires.m2tex", pack_path(), fs::copy_options::overwrite_existing, ec);
        if (ec) return fail(Problem::Save, "the pack could not be saved to " + pack_path() + ": " + ec.message());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.pack_version;
    }

    // 5. clean up: the captured textures, the models, the container and the
    // base image it was built on (unless the base was there before)
    set_stage(Stage::Cleaning, 0.97f);
    fs::remove_all(work, ec);
    if (ec) log("could not remove " + work + ": " + ec.message());
    run({"docker", "rmi", kImage});
    if (!base_there && !base.empty()) run({"docker", "rmi", base});
    log("done");
    set_stage(Stage::Done, 1.0f);
}

} // namespace app
