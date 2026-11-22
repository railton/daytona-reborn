#pragma once

#include "runtime/geo.h"

#include <string>
#include <vector>

namespace app {

// The optional pack is read from the user's machine once, at GPU startup.
class OpponentBodies {
public:
    static constexpr uint32_t atlas_bytes = 3 * 10 * (256 * 128 + 128 * 64 + 64 * 32 + 32 * 16) * 4;

    bool load(const std::string &directory);
    const std::vector<uint8_t> &atlas() const { return atlas_; }
    // A null result means the original scene can be used directly.
    const std::vector<rt::GeoPoly> *replace(const std::vector<rt::GeoPoly> &scene);

private:
    struct Face {
        uint8_t count = 0;
        uint8_t surface = 0;
        float shade = 1;
        std::array<std::array<float, 5>, 4> vertices{};
    };
    std::array<std::vector<Face>, 3> bodies_;
    std::vector<uint8_t> atlas_;
    std::vector<rt::GeoPoly> scene_;
};

} // namespace app
