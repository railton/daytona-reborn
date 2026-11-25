// The launcher: a Dear ImGui interface in the game's window. Game tab: the
// ROM set (browse, per-file verification), graphics API, fullscreen.
// Controls tab: every arcade control's key, gamepad and wheel/joystick
// binding (press to bind; a joystick axis is moved fully and let go, which
// calibrates it), live steering and pedal meters, dead zones. Settings are
// saved as they change.
#pragma once

#include "app/config.h"
#include "runtime/rom_import.h"

#include <SDL3/SDL.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

namespace app { class QualityProbe; }

namespace app {

// Fullscreen in Config::fullscreen_mode ("WxH@Hz", "*density" when not 1):
// the display's closest exclusive mode, or borderless at the desktop's ("").
void apply_fullscreen_mode(SDL_Window *window, const std::string &mode);

class Launcher {
public:
    Launcher(Config &cfg, SDL_Window *window);

    enum Result { Stay, StartGame, Resume, Reset, Quit, Back };
    // Returns true if the event was consumed (binding capture).
    bool handle_event(const SDL_Event &e);
    // Draw the interface (between ImGui::NewFrame and ImGui::Render).
    Result draw(bool game_running, const Devices &devices, bool from_home = false);
    // The home screen's fonts (HomeMenu): with them, the PC home's Settings
    // take its neon look (app/neon_ui.h), as the room's settings.
    void set_fonts(ImFont *display, ImFont *body) { display_font_ = display, body_font_ = body; }

    bool rom_ok() const { return rom_ok_; }
    // The PC home's ROM set screen: shown, and nothing else usable, until a
    // ROM set is verified; Settings' "Change..." opens it again (Back closes it then).
    bool rom_gate() const { return !rom_ok_ || gate_open_ || capture_gate(); }
    Result draw_rom_gate();
    const std::string &error() const { return error_; }
    void set_error(const std::string &e) { error_ = e; }
    void set_ffb_device(const char *kind) { ffb_device_ = kind; } // what force feedback is playing on
    void set_link_status(const std::string &s) { link_status_ = s; } // link play, for the Game tab
    void set_pacing_status(const std::string &s) { pacing_status_ = s; } // frame pacing now, for the Game tab
    // Automatic quality (app/quality.h): the measurement's window, over
    // everything else (modal): the graphics card, the progress (`shown`,
    // 0..1, paced by the caller), then the four levels with their times, the
    // recommended one picked; the player may pick another. Apply: quality_pick().
    enum class QualityChoice { None, Apply, Keep };
    QualityChoice draw_quality(const QualityProbe &probe, float shown, bool game_running);
    int quality_pick() const { return quality_pick_; } // app::QualityLevel
    // Settings' "Detect automatically" was pressed since the last call
    bool take_quality_request() { const bool r = quality_request_; quality_request_ = false; return r; }

private:
    ImFont *display_font_ = nullptr, *body_font_ = nullptr;
    void check_rom();
    void browse();
    void take_dialog_result(); // the file window's choice, from its thread
    bool language_combo(float width);
    bool gate_open_ = false;
    bool quality_request_ = false;
    int quality_pick_ = -1; // the level picked in draw_quality, -1 before its result
    static bool capture_gate(); // diagnostic: M2_CAPTURE_ROM_GATE=1
    static void SDLCALL dialog_done(void *self, const char *const *files, int filter);

    Config &cfg_;
    SDL_Window *window_;
    std::vector<rt::RomCheck> checks_;
    bool rom_ok_ = false;
    std::string rom_message_, error_;
    const char *ffb_device_ = "none";
    std::string link_status_ = "off";
    std::string pacing_status_;
    char link_next_buf_[256] = {};
    char path_buf_[1024] = {};

    std::mutex dialog_mutex_;
    std::string dialog_result_;
    bool dialog_pending_ = false;

    int capture_action_ = -1; // binding being captured
    enum CaptureKind { CaptureKey, CapturePad, CaptureJoy } capture_kind_ = CaptureKey;
    // joystick axis capture: every axis's value when the capture began; the
    // axis being moved, its rest value and the furthest it has gone
    std::map<std::pair<SDL_JoystickID, int>, int> joy_rest_;
    bool joy_tracking_ = false;
    SDL_JoystickID joy_id_ = 0;
    int joy_axis_ = 0, joy_from_ = 0, joy_extreme_ = 0;
    void start_capture(int action, CaptureKind kind, const Devices &devices);
};

} // namespace app
