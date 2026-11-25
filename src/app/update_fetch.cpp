#include "app/update_fetch.h"

#include <vector>

namespace app {

namespace {
// GitHub's answer for a release is a few kilobytes (its notes included);
// anything past this is not one.
constexpr size_t kMaxAnswer = 2 * 1024 * 1024;
} // namespace

void UpdateFetch::stop() {
    cancel_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (process_) SDL_KillProcess(process_, true);
    }
    if (worker_.joinable()) worker_.join();
}

bool UpdateFetch::start(const std::string &url) {
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_.reset();
    }
    worker_ = std::thread([this, url] {
        run(url);
        busy_ = false;
    });
    return true;
}

std::optional<UpdateFetch::Result> UpdateFetch::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<Result> r = std::move(result_);
    result_.reset();
    return r;
}

void UpdateFetch::run(std::string url) {
    Result r;
    // Nothing about the player: a generic User-Agent, the public API, no
    // cookies or credentials. -f: an HTTP error (404 for a private or
    // missing repository, 403 when rate limited) gives no output.
    const char *argv[] = {"curl", "-s", "-f", "--proto", "=https", "--max-time", "20", "--max-filesize", "2000000",
                          "-A", "daytona-reborn-update-check", "-H", "Accept: application/vnd.github+json",
                          url.c_str(), nullptr};
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
#ifdef SDL_PLATFORM_WINDOWS
    // curl.exe is a console program: no console window over the game. Its
    // exit code then always reads 0; the output alone tells.
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process *process = cancel_ ? nullptr : SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        r.error = std::string("cannot run curl: ") + SDL_GetError();
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(r);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = process;
    }
    std::string answer;
    bool too_long = false;
    if (SDL_IOStream *out = SDL_GetProcessOutput(process)) {
        char buf[4096];
        while (!cancel_) {
            const size_t n = SDL_ReadIO(out, buf, sizeof buf);
            if (n == 0) {
                if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                    SDL_Delay(10);
                    continue;
                }
                break; // the end, or an error
            }
            if (answer.size() + n > kMaxAnswer) {
                too_long = true;
                SDL_KillProcess(process, true);
                break;
            }
            answer.append(buf, n);
        }
    }
    if (cancel_) SDL_KillProcess(process, true); // the game is quitting
    int code = -1;
    SDL_WaitProcess(process, true, &code);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = nullptr;
    }
    SDL_DestroyProcess(process);
    if (too_long) r.error = "the answer is too long";
    else if (code != 0) r.error = "curl failed (exit " + std::to_string(code) + "; offline, or no such repository)";
    else if (answer.empty()) r.error = "no answer (offline, or no such repository)";
    else if (const auto release = update::parse_release(answer)) r.ok = true, r.release = *release;
    else r.error = "the answer holds no release version";
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = std::move(r);
}

} // namespace app
