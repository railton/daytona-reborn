// Custom cars: the project's cars/ folder (beside roms/), one folder per car in
// the format of docs/CUSTOM-CARS.md. Settings > Cars lists the folders that
// load; with none, the tab is not shown.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace app {

struct CustomCar {
    std::string folder; // the folder's name, what launcher.ini's player_car= keeps
    std::string name;   // car.ini's name=, else the folder's name
    std::string credit; // the credits file's first line, its links left out
};

struct CustomCarList {
    std::vector<CustomCar> cars;       // the cars that load, sorted by name
    std::vector<std::string> skipped;  // "FOLDER: why", one per folder left out
};

// The cars folder with a trailing separator: cars/ beside the .AppImage when
// run as one, else the project's cars/ (beside the build folder, as roms/),
// else cars/ in the working folder; "" when none exists.
std::string cars_dir();

// The folder holding the running .AppImage file, with a trailing separator
// ("" when not run as an AppImage): the game's own folders (roms/, cars/,
// textures/) sit beside it, the image itself being read-only.
inline std::string beside_appimage() {
    const char *image = std::getenv("APPIMAGE"); // set by the AppImage runtime: the .AppImage file
    if (!image || !*image) return {};
    const auto dir = std::filesystem::u8path(image).parent_path().u8string();
    return std::string(dir.begin(), dir.end()) + "/";
}

// Every folder of `dir` checked against the format: bodies.m2car and
// liveries.rgba as the renderer reads them (wheel.m2car, car.ini, preview.png
// and the credits optional). The checks are kept per folder until one of its
// files changes.
CustomCarList list_cars(const std::string &dir);

// Why the folder is not a car ("" when it loads), in English for the log.
std::string car_problem(const std::string &folder_path);

} // namespace app
