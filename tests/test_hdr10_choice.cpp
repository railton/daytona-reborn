// HDR10 composition choices, including an SDR display that still reports
// HDR10_ST2084 support. No window and no SDL.

#include "app/hdr10_choice.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "hdr10 choice: %s\n", message);
        ++failures;
    }
}
}

int main() {
    const app::Hdr10Choice off = app::choose_hdr10(false, true, true, "direct3d12");
    check(off.composition == app::Hdr10Composition::Sdr, "not requested stays SDR");
    check(off.reason == "not requested", "not requested reason");

    const app::Hdr10Choice on = app::choose_hdr10(true, true, true, "direct3d12");
    check(on.composition == app::Hdr10Composition::Hdr10St2084, "HDR display keeps HDR10_ST2084");
    check(on.reason == "requested and supported", "supported reason");

    const app::Hdr10Choice sdr_display = app::choose_hdr10(true, true, false, "direct3d12");
    check(sdr_display.composition == app::Hdr10Composition::Sdr, "SDR display stays SDR");
    check(sdr_display.reason == "requested but the display is not in HDR", "SDR display reason");

    const app::Hdr10Choice refused = app::choose_hdr10(true, false, true, "direct3d12");
    check(refused.composition == app::Hdr10Composition::Sdr, "refused request stays SDR");
    check(refused.reason == "requested but refused by the display or the driver (direct3d12)",
          "refused reason names the driver");

    const app::Hdr10Choice unnamed = app::choose_hdr10(true, false, false, nullptr);
    check(unnamed.composition == app::Hdr10Composition::Sdr, "missing driver still stays SDR");
    check(unnamed.reason == "requested but refused by the display or the driver (unknown)",
          "missing driver reason");

    if (failures) return 1;
    std::printf("hdr10 choice: %s, %s\n", app::hdr10_composition_name(off.composition), off.reason.c_str());
    std::printf("hdr10 choice: %s, %s\n", app::hdr10_composition_name(on.composition), on.reason.c_str());
    std::printf("hdr10 choice: %s, %s\n", app::hdr10_composition_name(sdr_display.composition),
                sdr_display.reason.c_str());
    std::printf("hdr10 choice: %s, %s\n", app::hdr10_composition_name(refused.composition),
                refused.reason.c_str());
    std::puts("hdr10 choice: not requested, HDR display, SDR display and refused passed");
    return 0;
}
