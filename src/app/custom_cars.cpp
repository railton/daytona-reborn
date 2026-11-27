#include "app/custom_cars.h"

#include "app/gpu/opponent_bodies.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace app {

namespace fs = std::filesystem;

std::string cars_dir() {
    const char *base = SDL_GetBasePath();
    const std::string image = beside_appimage();
    for (const std::string &dir : {image.empty() ? std::string() : image + "cars/", std::string(base ? base : "") + "../cars/",
                                   std::string("cars/")}) {
        if (dir.empty()) continue;
        std::error_code ec;
        if (fs::is_directory(fs::u8path(dir), ec)) return dir;
    }
    return {};
}

std::string car_problem(const std::string &folder_path) {
    const fs::path root = fs::u8path(folder_path);
    std::error_code ec;
    if (!fs::exists(root / "bodies.m2car", ec)) return "no bodies.m2car";
    {
        std::ifstream f(root / "bodies.m2car");
        std::string tag;
        if (!(f >> tag) || tag != "M2CAR1") return "bodies.m2car does not start with M2CAR1";
    }
    if (!fs::exists(root / "liveries.rgba", ec)) return "no liveries.rgba";
    const auto size = fs::file_size(root / "liveries.rgba", ec);
    if (ec || size != OpponentBodies::atlas_bytes) {
        char text[160];
        std::snprintf(text, sizeof text, "liveries.rgba is %llu bytes, %u expected", (unsigned long long)(ec ? 0 : size),
                      unsigned(OpponentBodies::atlas_bytes));
        return text;
    }
    OpponentBodies body; // the renderer's own reader: what it refuses, the list leaves out
    if (!body.load(folder_path, 32768))
        return body.error().empty() ? "bodies.m2car is not three bodies of 1 to 32768 faces (docs/CUSTOM-CARS.md)"
                                    : body.error() + " (docs/CUSTOM-CARS.md)";
    return {};
}

namespace {

// a folder's files' sizes and times: its check is made again when they change
std::string stamp(const fs::path &folder) {
    std::string s;
    for (const char *file : {"bodies.m2car", "liveries.rgba", "wheel.m2car"}) {
        const fs::path p = folder / file;
        std::error_code size_ec, time_ec;
        const auto size = fs::file_size(p, size_ec);
        const auto time = fs::last_write_time(p, time_ec);
        // the clock's count is a wider integer on Windows: as long long for to_string
        s += std::to_string(size_ec ? 0ull : (unsigned long long)size) + ":" +
             std::to_string(time_ec ? 0ll : (long long)time.time_since_epoch().count()) + ";";
    }
    return s;
}

std::string first_line(const fs::path &file) {
    std::string line;
    if (std::ifstream f(file); f) std::getline(f, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

} // namespace

CustomCarList list_cars(const std::string &dir) {
    static std::map<std::string, std::pair<std::string, std::string>> checked; // path -> (stamp, problem)
    static std::map<std::string, std::string> logged;                          // path -> the problem told
    CustomCarList list;
    if (dir.empty()) return list;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(fs::u8path(dir), ec)) {
        if (!e.is_directory(ec)) continue;
        const auto u8 = e.path().filename().u8string(); // the folder's name as UTF-8 (accents on Windows)
        const std::string folder(u8.begin(), u8.end());
        if (folder.empty() || folder[0] == '.') continue;
        const auto u8path = e.path().u8string();
        const std::string path(u8path.begin(), u8path.end());
        auto &entry = checked[path];
        if (const std::string now = stamp(e.path()); entry.first != now) entry = {now, car_problem(path)};
        if (!entry.second.empty()) {
            list.skipped.push_back(folder + ": " + entry.second);
            if (logged[path] != entry.second)
                std::fprintf(stderr, "daytona: cars/%s left out: %s\n", folder.c_str(), entry.second.c_str());
            logged[path] = entry.second;
            continue;
        }
        logged.erase(path);
        CustomCar c{folder, folder, {}};
        if (std::ifstream ini(e.path() / "car.ini"); ini)
            for (std::string line; std::getline(ini, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("name=", 0) == 0 && line.size() > 5) c.name = line.substr(5);
            }
        for (const char *credits : {"CREDITS.txt", "CREDITOS.txt"})
            if (fs::exists(e.path() / credits, ec)) {
                c.credit = first_line(e.path() / credits);
                // the links left out: the card has room for the title and the author
                for (size_t at; (at = c.credit.find(" (http")) != std::string::npos;) {
                    const size_t end = c.credit.find(')', at);
                    c.credit.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
                }
                break;
            }
        list.cars.push_back(std::move(c));
    }
    std::sort(list.cars.begin(), list.cars.end(), [](const CustomCar &a, const CustomCar &b) { return a.name < b.name; });
    std::sort(list.skipped.begin(), list.skipped.end());
    return list;
}

} // namespace app
