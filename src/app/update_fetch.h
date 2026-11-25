// The update check's request (app/update_check.h): the system's curl, run
// in a background thread with SDL_CreateProcessWithProperties, its output
// read and parsed there. One request per start(); take() hands its result
// to the main thread once.
#pragma once

#include "app/update_check.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace app {

class UpdateFetch {
public:
    struct Result {
        bool ok = false;         // an answer with a version in it
        update::Release release; // when ok
        std::string error;       // English, for the log, when not ok
    };
    ~UpdateFetch() { stop(); }
    // A request still running stopped (curl killed), its thread joined: at
    // exit, before SDL_Quit.
    void stop();
    // GET url (https only) in the background; false while one runs.
    bool start(const std::string &url);
    bool busy() const { return busy_; }
    // The finished request's result, once.
    std::optional<Result> take();

private:
    void run(std::string url);
    std::thread worker_;
    std::atomic<bool> busy_{false}, cancel_{false};
    std::mutex mutex_;
    std::optional<Result> result_;
    SDL_Process *process_ = nullptr; // under mutex_: killed when the game quits mid-request
};

} // namespace app
