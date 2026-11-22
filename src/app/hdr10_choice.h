// HDR10 composition choice. No SDL and no window: the caller reports whether
// HDR10_ST2084 is supported, whether the display is in HDR, and which GPU
// driver answered.
#pragma once

#include <string>

namespace app {

enum class Hdr10Composition {
    Sdr,
    Hdr10St2084,
};

struct Hdr10Choice {
    Hdr10Composition composition = Hdr10Composition::Sdr;
    std::string reason;
};

inline const char *hdr10_composition_name(Hdr10Composition composition) {
    return composition == Hdr10Composition::Hdr10St2084 ? "HDR10_ST2084" : "SDR";
}

// HDR10_ST2084 is kept only when requested, the composition is supported, and
// the window's display is actually in HDR. Support alone is not enough.
inline Hdr10Choice choose_hdr10(bool requested, bool st2084_supported, bool display_hdr,
                                const char *driver) {
    if (!requested) return {Hdr10Composition::Sdr, "not requested"};
    if (!st2084_supported) {
        const char *name = driver && driver[0] ? driver : "unknown";
        return {Hdr10Composition::Sdr,
                std::string("requested but refused by the display or the driver (") + name + ")"};
    }
    if (!display_hdr) return {Hdr10Composition::Sdr, "requested but the display is not in HDR"};
    return {Hdr10Composition::Hdr10St2084, "requested and supported"};
}

}
