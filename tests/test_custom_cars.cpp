// The cars/ folder's checks (app/custom_cars.h): a folder in the format is
// listed with its name and credit, every other one left out with its reason.
#include "app/custom_cars.h"
#include "app/gpu/opponent_bodies.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void write(const fs::path &p, const std::string &text) { std::ofstream(p, std::ios::binary) << text; }

static void liveries(const fs::path &p, size_t bytes) {
    std::ofstream f(p, std::ios::binary);
    const std::string zeros(bytes, '\0');
    f.write(zeros.data(), std::streamsize(zeros.size()));
}

// three bodies of one triangle each: the smallest bodies.m2car that loads
static const char *k_bodies = "M2CAR1\n"
                              "1\n3 0 0  0 0 0 0 0  1 0 0 1 0  0 0 1 0 1\n"
                              "1\n3 0 0  0 0 0 0 0  1 0 0 1 0  0 0 1 0 1\n"
                              "1\n3 0 0  0 0 0 0 0  1 0 0 1 0  0 0 1 0 1\n";

int main() {
    int failures = 0;
    auto check = [&](bool value, const char *message) {
        if (!value) ++failures, std::fprintf(stderr, "FAIL: %s\n", message);
    };
    const fs::path dir = fs::temp_directory_path() / "test_custom_cars";
    fs::remove_all(dir);
    fs::create_directories(dir);

    fs::create_directories(dir / "good");
    write(dir / "good" / "bodies.m2car", k_bodies);
    liveries(dir / "good" / "liveries.rgba", app::OpponentBodies::atlas_bytes);
    write(dir / "good" / "car.ini", "glass_opacity=0.75\r\nname=A Good Car\r\n");
    write(dir / "good" / "CREDITS.txt", "\"Model\" by Someone (https://example.org/someone)\nmore\n");

    fs::create_directories(dir / "short_atlas");
    write(dir / "short_atlas" / "bodies.m2car", k_bodies);
    liveries(dir / "short_atlas" / "liveries.rgba", 1000);

    fs::create_directories(dir / "no_body");
    liveries(dir / "no_body" / "liveries.rgba", app::OpponentBodies::atlas_bytes);

    fs::create_directories(dir / "bad_body");
    write(dir / "bad_body" / "bodies.m2car", "M2CAR1\n1\n3 0 0\n");
    liveries(dir / "bad_body" / "liveries.rgba", app::OpponentBodies::atlas_bytes);

    // a face of 5 corners: the whole body refused, the log says which face and why
    fs::create_directories(dir / "pentagon");
    write(dir / "pentagon" / "bodies.m2car", "M2CAR1\n1\n5 0 1  0 0 0 0 0  1 0 0 1 0  1 1 0 1 1  0 1 0 0 1  0 0.5 0 0 0.5\n");
    liveries(dir / "pentagon" / "liveries.rgba", app::OpponentBodies::atlas_bytes);

    fs::create_directories(dir / ".hidden");
    write(dir / "game-car.png", "not a folder");

    const std::string root = dir.string() + "/";
    const app::CustomCarList list = app::list_cars(root);
    check(list.cars.size() == 1, "one car in the format");
    if (!list.cars.empty()) {
        check(list.cars[0].folder == "good", "the folder's name kept");
        check(list.cars[0].name == "A Good Car", "car.ini's name=, its CR left out");
        check(list.cars[0].credit == "\"Model\" by Someone", "the credit's first line without its link");
    }
    check(list.skipped.size() == 4, "four folders left out (not the hidden one, not a file)");
    auto skipped = [&](const char *prefix) {
        for (const std::string &s : list.skipped)
            if (s.rfind(prefix, 0) == 0) return true;
        return false;
    };
    check(skipped("bad_body: bodies.m2car, body 1 of 3: face 1: a corner unreadable"), "a broken body is left out, its face named");
    check(skipped("pentagon: bodies.m2car, body 1 of 3: face 1: 5 corners (3 or 4 only"), "a face of 5 corners named");
    check(skipped("no_body: no bodies.m2car"), "a folder without bodies.m2car is left out");
    check(skipped("short_atlas: liveries.rgba is 1000 bytes"), "a short atlas is left out");
    check(app::list_cars("").cars.empty(), "no folder: no car");

    // a fix is seen: the checks are made again when a file changes
    liveries(dir / "short_atlas" / "liveries.rgba", app::OpponentBodies::atlas_bytes);
    check(app::list_cars(root).cars.size() == 2, "a folder fixed is listed");

    fs::remove_all(dir);
    if (!failures) std::printf("custom cars: all checks passed\n");
    return failures ? 1 : 0;
}
