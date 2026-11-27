// Launcher settings, saved to launcher.ini in the user's data folder (SDL's
// pref path): the ROM set, the GPU backend, fullscreen, audio volume, and
// the control bindings. Plain key=value lines, so it can be edited by hand.
#pragma once

#include "app/controls.h"

#include <string>
#include <cstdint>
#include <iosfwd>

namespace app {

struct Config {
    std::string rom_path;
    std::string gpu;          // "" (automatic), vulkan, direct3d12, metal
    std::string renderer = "software"; // the 3D: software (CPU, exact) or hardware (SDL_GPU)
    bool fullscreen = true;  // a game's expected start (borderless at the desktop's mode below); F11 toggles
    bool hide_cursor = true; // no mouse cursor while the game plays; it shows again in the launcher
    // Exclusive fullscreen in this mode ("WxH@Hz", with "*density" when not
    // 1), or "" for borderless at the desktop's mode.
    std::string fullscreen_mode;
    // Frame pacing (#7, app/pacing.h): the arcade's speed on any display; the
    // smooth pacing, on, acts only on a display at 57.52 or 115.05 Hz.
    bool pace_smooth = true;        // a display at a multiple of 57.52 Hz: one frame per refresh(es)
    bool pace_sync_display = false; // the game at a rate dividing the refresh (60 on 60/120/240 Hz: 4% fast)
    bool pace_vrr = false;          // each frame held to 1/57.52 s, for a variable-refresh display
    bool skip_launcher = false; // start the game straight away (as --autostart); Esc still opens the launcher
    // Hold the cabinet's Test button for 3 s once the game runs (rt::TestHold),
    // for the test menu without an F2 key: mobile, consoles. Not saved; it
    // clears itself when done.
    bool hold_test = false;
    float volume = 0.8f;      // 0..1
    bool mute = false;
    float music_volume = 1.0f;   // 0..1: the music against the effects (both 1: as the arcade mixes them)
    float effects_volume = 1.0f; // 0..1
    bool native_audio = false; // applies on reset; reference remains the default
    // Enhancements (off by default).
    std::string language;      // interface (app/i18n.h): "fr", "en", ...; "" the system's (first run)
    std::string aspect;        // "4:3" (the cabinet's, in every mode), "" (automatic: the window's shape in the
                               // native mode, else the original), "16:10", "16:9", "21:9"
    bool arcade_43() const { return aspect == "4:3"; }
    bool hud_edges = false;    // with widescreen: lap times, position and maps at the screen edges
    bool stretch_backdrop = false; // with widescreen, in-game: the tile backdrop stretched across the width, else plain sky
    int draw_distance = 0;     // scenery: 0 = the game's own, -2..+2 (rt::Enhance)
    bool scenery_3d = false;   // fir trees in volume, filled stands, Dinosaur Canyon piers (next start)
    bool interpolate = false;  // pose smoothing between game frames, as --interpolate-poses (next start)
    bool smooth_online_cars = true; // online: the other players' cars drawn smoothly (Geo::smooth_remote_cars)
    bool cars_near_model = false; // every opponent drawn with its near model (next start)
    int msaa = 1;  // the hardware renderer's antialiasing: 1 (off), 2, 4, 8 samples (next start)
    int aniso = 1; // its texture filtering: 1 the game's own, 2..16 anisotropic
    bool smooth_cars = false;     // its cars' light blended across their faces (GpuRenderer::set_car_shading)
    bool car_reflections = false; // its cars' paint reflecting the sky and the sun
    bool sharp_car_textures = false; // its cars' numbers and logos crisp up close
    bool car_shadows = false;        // its cars' shadows from the game's sun (shadow map)
    bool replace_game_shadow = true; // with car_shadows: the game's own checker shadow under the car left out
    bool bloom = false;              // the display: the bright parts glow (GpuRenderer::present)
    bool crt_filter = false;         // the display: an arcade monitor's scanlines, grille and corners
    bool smooth_2d = false;          // the menus, HUD and text magnified smooth (GpuRenderer::set_smooth_2d)
    bool hires_textures = false;     // draw the high-resolution texture pack (HiresMaker::pack_path: the project's textures/)
    std::string player_car;          // a body from the project's cars/NAME/ for the player's car ("" the game's)
    bool hires_processor = false;    // make that pack on the processor (slow) instead of the graphics card
    int bloom_strength = 35;         // the glow added, percent (10..100)
    int crt_strength = 100;          // the monitor's look, percent (20..100)
    bool quality_checked = false;    // the automatic quality measured once (app/quality.h; offered at the first start)
    bool sun_shadows = false;  // ray-traced sun shadows (Direct3D 12, ray tracing tier 1.1; next start)
    bool sky_light = false;    // with the shadows: the sky's light, hidden under and between things (next start)
    int sun_height = 0;        // the shadows' sun above the horizon, degrees (10..80); 0 = the game's own
    // Display curve (Direct3D 12): 0 = the PC sRGB curve (the reference);
    // 2.2..2.6 = the game's levels shown as on an arcade CRT. Restart required.
    float display_gamma = 0.0f;
    // Colour saturation of the final image (Direct3D 12): 1 = unchanged, 0.5..1.5.
    float display_saturation = 1.0f;
    uint32_t draw_budget = 0;   // 0 Automatic; positive Custom allowance (runtime/scenery.h)
    int draw_mode = 0;         // 0 double buffered (every frame), 1 single buffered (every 2nd), 2 every third frame
    // the native mode: the screen's (window's) own resolution, Direct3D 12 and
    // Vulkan. Until 0.21 it drew twice the screen each way (3840 x 2160 on a
    // 1080p screen) and kNativeDirect was the screen's: both read as this now.
    static constexpr int kNativeResolution = 0;
    static constexpr int kNativeDirect = 5;
    int supersampling = 1;     // 1-4 times arcade resolution (at most the screen's), or the native mode
    bool uses_native_resolution() const {
        return supersampling == kNativeResolution || supersampling == kNativeDirect;
    }
    static constexpr double kMaxAspect = 21.0 / 9.0;
    double aspect_ratio() const; // width / height; 0 = original
    Controls controls;
    // Online play (app/relay_link.h): through a daytona-relay server, the
    // home menu's Online screen. The player's name, the last server used.
    std::string online_name;
    std::string online_server;
    std::string online_password;
    std::string online_room; // the last room's settings its creator chose, "key:value,..."
    // The gearbox the player races with (the home screen's choice): solo, and
    // online, where the game answers its gearbox screen with it on its own.
    bool gearbox_manual = false;
    // Solo races from the PC home: the cabinet's country (0 USA: English and
    // mph, 1 JPN: Japanese and km/h, 2 EXPORT: English and km/h, shown as
    // "International (km/h)", the default) and difficulty (0 easy .. 3 hardest).
    int solo_country = 2, solo_difficulty = 1;
    // Logs: what the game prints, kept in the logs folder beside roms/ (the
    // last 5 sessions too: logs_dir(), log_name()), for when something goes wrong.
    bool write_logs = true;
    // "A new version is out" (app/update_check.h): GitHub's latest release
    // asked at most once a day from the PC home. The last check (Unix
    // seconds), the latest release seen and its page, and the version the
    // player dismissed the notice for.
    bool update_check = true;
    int64_t update_checked_at = 0;
    std::string update_latest;     // its tag, "v0.19.0"
    std::string update_latest_url; // https://github.com/... only
    std::string update_dismissed;  // a tag: no notice for that version
    float ffb_strength = 0.7f; // force feedback (the drive board) on the steering device: 0 off .. 1
    bool ffb_invert = false;   // turn the wheel the other way
    bool ffb_log = false;      // log the force feedback device, failures and the commands (in the game's log)
    bool ffb_centring_override = false; // Linux: the game's centring worked out here, sent as constant force (#16)
    // Linux and macOS: Logitech wheels through the system's driver, not SDL's
    // own (its HIDAPI lg4ff driver, off on Windows already). Some, like the
    // original Driving Force, send SDL's driver nothing (issue #4). On by
    // default on Linux, whose kernel driver gives the wheels force feedback;
    // off on macOS, where only SDL's driver does. Applies at start-up; SDL
    // gives the wheel another GUID, so it is bound again.
#ifdef __linux__
    bool legacy_logitech_wheels = true;
#else
    bool legacy_logitech_wheels = false;
#endif

    Config() { controls.set_defaults(); }
    static std::string path();  // <pref path>/launcher.ini
    // --profile NAME: a separate data folder (settings, EEPROM, backup RAM),
    // e.g. a second cabinet on the same computer for link play.
    static inline std::string profile;
    static std::string pref_dir(); // where this set's (and profile's) data lives, with a trailing separator
    // The logs: a logs/ folder beside the game's roms/ (and cars/, textures/):
    // beside the .AppImage, beside the program (the Windows package), the
    // project's (build-daytona/../), or the working folder's; made when
    // missing. pref_dir() when there is no roms/ folder, or it cannot be written.
    static std::string logs_dir();
    // This set's and profile's log name, without .log: "daytona-pc-solo" (pref_dir's own name).
    static std::string log_name();
    void load();
    void save() const;
    void read(std::istream &input);
    void write(std::ostream &output) const;
};

} // namespace app
