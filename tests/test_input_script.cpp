#include "../tools/common/input_script.h"

#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string root = argv[1];
    tools::Script script;
    script.load(root + "/valid.txt");
    const auto at_zero = script.at(0), at_one = script.at(1), at_two = script.at(2);
    if (at_zero.accel != 0xe0 || at_zero.steer != 0x20 ||
        at_one.accel != 0xe0 || at_one.steer != 0x70 || at_one.in0 != 0xfe ||
        at_two.accel != 0x20 || at_two.steer != 0x80) {
        std::fputs("separate assignments, inline comment or last-line override changed\n", stderr);
        return 1;
    }
    try {
        tools::Script invalid;
        invalid.load(root + "/ambiguous.txt");
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()).ends_with(":2: expected one input assignment per line")) return 0;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::fputs("second assignment was silently ignored\n", stderr);
    return 1;
}
