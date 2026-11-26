// Custom cars: the project's cars/ folder (beside roms/), one folder per car in
// the format of docs/CUSTOM-CARS.md. Settings > Cars lists the folders that
// load; with none, the tab is not shown.
#pragma once

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

// The cars folder with a trailing separator: the project's cars/ (beside the
// build folder, as roms/), else cars/ in the working folder; "" when neither
// exists.
std::string cars_dir();

// Every folder of `dir` checked against the format: bodies.m2car and
// liveries.rgba as the renderer reads them (wheel.m2car, car.ini, preview.png
// and the credits optional). The checks are kept per folder until one of its
// files changes.
CustomCarList list_cars(const std::string &dir);

// Why the folder is not a car ("" when it loads), in English for the log.
std::string car_problem(const std::string &folder_path);

} // namespace app
