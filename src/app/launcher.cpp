#include "app/launcher.h"
#include "app/i18n.h"
#include "app/rom_file.h"
#ifdef SDL_PLATFORM_IOS
#include "rom_picker.h"
#endif
#include "runtime/game_loop.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace app {

namespace {

std::string mode_key(const SDL_DisplayMode &m) {
    char s[64];
    if (m.pixel_density == 1.0f) std::snprintf(s, sizeof s, "%dx%d@%.3f", m.w, m.h, double(m.refresh_rate));
    else std::snprintf(s, sizeof s, "%dx%d@%.3f*%.2f", m.w, m.h, double(m.refresh_rate), double(m.pixel_density));
    return s;
}

std::string mode_label(const SDL_DisplayMode &m) {
    char s[64];
    std::snprintf(s, sizeof s, "%d x %d, %.2f Hz%s", m.w, m.h, double(m.refresh_rate), m.pixel_density == 1.0f ? "" : " (HiDPI)");
    return s;
}

} // namespace

void apply_fullscreen_mode(SDL_Window *window, const std::string &key) {
    int w = 0, h = 0;
    float hz = 0, density = 1;
    SDL_DisplayMode mode;
    if (!key.empty() && std::sscanf(key.c_str(), "%dx%d@%f*%f", &w, &h, &hz, &density) >= 3 &&
        SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), w, h, hz, density > 1, &mode))
        SDL_SetWindowFullscreenMode(window, &mode);
    else
        SDL_SetWindowFullscreenMode(window, nullptr); // borderless, at the desktop's mode
}

Launcher::Launcher(Config &cfg, SDL_Window *window) : cfg_(cfg), window_(window) {
    std::snprintf(link_next_buf_, sizeof link_next_buf_, "%s", cfg_.link_next.c_str());
#ifdef SDL_PLATFORM_ANDROID
    // Older mobile builds saved the picker result (content://...) directly.
    // Do not reopen such a URI during startup: Android's temporary document
    // grant may be stale, and the picker/JNI path is not part of launcher
    // construction. Ask the user to browse again and import it then.
    if (RomFile::is_content_uri(cfg_.rom_path)) {
        cfg_.rom_path.clear();
        path_buf_[0] = '\0';
        cfg_.save();
        rom_message_ = TX("Choose the ROM set again with Browse.");
        return;
    }
#endif
    std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
    check_rom();
}

void Launcher::check_rom() {
    checks_.clear();
    rom_ok_ = false;
    if (cfg_.rom_path.empty()) {
        char text[256];
        std::snprintf(text, sizeof text, TX("Choose the %s ROM set (.zip or .7z)."), M2_ROMSET);
        rom_message_ = text;
        return;
    }
    try {
        // Android's picker grants access to a content URI, not a raw /sdcard
        // path. Stage it through SDL's Android reader before the plain-file
        // archive code verifies it. Invalid imports never replace a good copy.
        RomFile selected(cfg_.rom_path);
        checks_ = rt::check_rom_set(selected.path());
        int good = 0;
        for (const auto &c : checks_) good += c.ok;
        const bool verified = !checks_.empty() && good == int(checks_.size());
        if (verified) {
            std::string path = selected.commit();
            // Keep ordinary paths absolute, including the saved Android copy.
            std::error_code ec;
            const auto abs = std::filesystem::absolute(path, ec);
            if (!ec && std::filesystem::exists(abs, ec)) path = abs.lexically_normal().string();
            if (cfg_.rom_path != path) {
                cfg_.rom_path = path;
                std::snprintf(path_buf_, sizeof path_buf_, "%s", path.c_str());
                cfg_.save();
            }
        }
        // Do not enable Start if committing the imported archive failed.
        rom_ok_ = verified;
        char text[256];
        if (rom_ok_) std::snprintf(text, sizeof text, TX("All %d files are verified."), good);
        else std::snprintf(text, sizeof text, TX("%d of %d files missing or wrong: this is not the %s set."),
                           int(checks_.size()) - good, int(checks_.size()), M2_ROMSET);
        rom_message_ = text;
    } catch (const std::exception &e) {
        rom_message_ = e.what();
    }
}

void SDLCALL Launcher::dialog_done(void *self, const char *const *files, int) {
    auto *l = static_cast<Launcher *>(self);
    std::lock_guard<std::mutex> g(l->dialog_mutex_);
    l->dialog_pending_ = false;
    if (files && files[0]) {
        l->dialog_result_ = files[0];
        l->error_.clear();
    } else if (!files) l->error_ = std::string(TX("ROM choice: ")) + SDL_GetError();
}

void Launcher::browse() {
    static SDL_DialogFileFilter filters[2]; // kept until the dialog answers; in the current language
    filters[0] = {TX("ROM set (zip, 7z)"), "zip;7z"};
    filters[1] = {TX("All files"), "*"};
    dialog_pending_ = true;
#ifdef SDL_PLATFORM_IOS
    ios_browse_rom(window_, dialog_done, this);
#else
    SDL_ShowOpenFileDialog(dialog_done, this, window_, filters, 2, cfg_.rom_path.empty() ? nullptr : cfg_.rom_path.c_str(), false);
#endif
}

void Launcher::start_capture(int action, CaptureKind kind, const Devices &devices) {
    capture_action_ = action;
    capture_kind_ = kind;
    joy_tracking_ = false;
    joy_rest_.clear();
    if (kind == CaptureJoy)
        for (SDL_Joystick *j : devices.joys)
            for (int i = 0; i < SDL_GetNumJoystickAxes(j); i++) joy_rest_[{SDL_GetJoystickID(j), i}] = SDL_GetJoystickAxis(j, i);
}

bool Launcher::handle_event(const SDL_Event &e) {
    if (capture_action_ < 0) return false;
    Binding &b = cfg_.controls.bind[capture_action_];
    auto done = [&] {
        capture_action_ = -1;
        joy_tracking_ = false;
        cfg_.save();
    };
    auto guid_of = [](SDL_JoystickID id) {
        char g[64] = "";
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), g, sizeof g);
        return std::string(g);
    };
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            capture_action_ = -1; // cancel
            joy_tracking_ = false;
        } else if (capture_kind_ == CaptureKey) {
            b.key = (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE) ? SDL_SCANCODE_UNKNOWN
                                                                                                         : e.key.scancode;
            done();
        }
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b.pad.kind = PadInput::Button;
        b.pad.index = e.gbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 20000) {
        b.pad.kind = PadInput::Axis;
        b.pad.index = e.gaxis.axis;
        b.pad.dir = e.gaxis.value > 0 ? 1 : -1;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !joy_tracking_) {
        b.joy = JoyInput{};
        b.joy.kind = JoyInput::Button;
        b.joy.guid = guid_of(e.jbutton.which);
        b.joy.index = e.jbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && !joy_tracking_) {
        const int v = e.jhat.value;
        if (v == SDL_HAT_UP || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT || v == SDL_HAT_RIGHT) {
            b.joy = JoyInput{};
            b.joy.kind = JoyInput::Hat;
            b.joy.guid = guid_of(e.jhat.which);
            b.joy.index = e.jhat.hat;
            b.joy.mask = v;
            done();
        }
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        // An axis binds once it has moved well away from where it rested and
        // come back: its rest value and the furthest it went are its range.
        const auto key = std::make_pair(e.jaxis.which, int(e.jaxis.axis));
        const auto it = joy_rest_.find(key);
        const int rest = it != joy_rest_.end() ? it->second : 0, v = e.jaxis.value;
        if (!joy_tracking_) {
            if (std::abs(v - rest) > 12000) {
                joy_tracking_ = true;
                joy_id_ = e.jaxis.which, joy_axis_ = e.jaxis.axis, joy_from_ = rest, joy_extreme_ = v;
            }
        } else if (e.jaxis.which == joy_id_ && e.jaxis.axis == joy_axis_) {
            if (std::abs(v - joy_from_) > std::abs(joy_extreme_ - joy_from_)) joy_extreme_ = v;
            if (std::abs(v - joy_from_) * 10 < std::abs(joy_extreme_ - joy_from_) * 3) { // let go: back within 30%
                b.joy = JoyInput{};
                b.joy.kind = JoyInput::Axis;
                b.joy.guid = guid_of(joy_id_);
                b.joy.index = joy_axis_;
                b.joy.rest = joy_from_;
                b.joy.full = joy_extreme_;
                done();
            }
        }
        return true;
    }
    return e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_JOYSTICK_BUTTON_UP;
}

Launcher::Result Launcher::draw(bool game_running, const Devices &devices, bool from_home) {
    SDL_Gamepad *pad = devices.pad;
    {
        std::lock_guard<std::mutex> g(dialog_mutex_);
        if (!dialog_result_.empty()) {
            cfg_.rom_path = dialog_result_;
            dialog_result_.clear();
            std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
            cfg_.save();
            check_rom();
        }
    }

    Result result = Stay;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
#ifdef SDL_PLATFORM_IOS
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(window_, &safe) && safe.w > 0 && safe.h > 0) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + safe.x, vp->Pos.y + safe.y));
        ImGui::SetNextWindowSize(ImVec2(float(safe.w), float(safe.h)));
    }
#endif
    ImGui::SetNextWindowBgAlpha(game_running ? 0.85f : 1.0f);
    ImGui::Begin("Daytona USA", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos();

    if (from_home) {
        if (ImGui::Button(TX("Back to the home screen"))) result = Back;
        ImGui::SameLine();
    }
    ImGui::TextUnformatted("DAYTONA USA");
    ImGui::SameLine();
    ImGui::TextDisabled(TX("  static recompilation, native engine"));
    {   // the interface language, each in its own script (app/i18n.h)
        const auto &langs = i18n::languages();
        size_t sel = 0;
        for (size_t i = 0; i < langs.size(); ++i)
            if (i18n::current() == langs[i].code) sel = i;
        // only the languages with a translation next to the game (and French)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("Langue / Language", langs[sel].name)) {
            for (size_t i = 0; i < langs.size(); ++i)
                if (i18n::available(langs[i].code) && ImGui::Selectable(langs[i].name, i == sel) && i != sel) {
                    cfg_.language = langs[i].code;
                    cfg_.save();
                    i18n::set(cfg_.language);
                    check_rom(); // its message, in the new language
                }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(TX("Game"))) {
            ImGui::Spacing();
            ImGui::TextUnformatted(TX("ROM set"));
#ifdef SDL_PLATFORM_ANDROID
            ImGui::TextWrapped(TX("Browse gives access to your ZIP/7z. A verified copy is kept in the app."));
#endif
            ImGui::SetNextItemWidth(-200);
            if (ImGui::InputText("##rom", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::SameLine();
            if (ImGui::Button(TX("Browse..."), ImVec2(100, 0)) && !dialog_pending_) browse();
            ImGui::SameLine();
            if (ImGui::Button(TX("Check"), ImVec2(90, 0))) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::TextColored(rom_ok_ ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1), "%s", rom_message_.c_str());
            if (!error_.empty()) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
            }
            if (!checks_.empty() && ImGui::TreeNode(TX("Files"))) {
                if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const auto &c : checks_) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.file.c_str());
                        ImGui::TableNextColumn();
                        if (c.ok) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "ok");
                        else ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", c.problem.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Display"));
            {   // the picture's format first, easy to find (viewers' request, Daytona-Reborn issues 1 and 2)
                const char *aspects[] = {TX("Arcade 4:3 (the original cabinet)"), TX("Automatic"), "16:10", "16:9",
                                         TX("21:9 (ultrawide)")};
                static const char *aspect_ids[] = {"4:3", "", "16:10", "16:9", "21:9"};
                int aspect = 1;
                for (int i = 0; i < 5; i++)
                    if (cfg_.aspect == aspect_ids[i]) aspect = i;
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo(TX("Picture format"), &aspect, aspects, 5)) {
                    cfg_.aspect = aspect_ids[aspect];
                    cfg_.save();
                }
                ImGui::SetItemTooltip("%s", TX("Arcade 4:3: the cabinet's framing, centred, with bars at the sides of a wide screen.\n"
                                               "Automatic: the window's shape with the native renderer, else the original 4:3.\n"
                                               "16:10, 16:9, 21:9: the scene widens at the sides, the car keeps its proportions."));
            }
            const char *apis[] = {TX("Automatic"), "Vulkan", "Direct3D 12", "Metal"};
            static const char *api_ids[] = {"", "vulkan", "direct3d12", "metal"};
            int api = 0;
            for (int i = 0; i < 4; i++)
                if (cfg_.gpu == api_ids[i]) api = i;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("Graphics API (restart the game)"), &api, apis, 4)) {
                cfg_.gpu = api_ids[api];
                cfg_.save();
            }
            const char *renderers[] = {TX("Software (exact)"), TX("Hardware (experimental)")};
            int rd = cfg_.renderer == "hardware" ? 1 : 0;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("Renderer"), &rd, renderers, 2)) {
                cfg_.renderer = rd ? "hardware" : "software";
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Software: the 3D is computed by the processor, as on the arcade board. Hardware:\n"
                                   "by the graphics card; in development, very close but not yet pixel exact."));
            if (ImGui::Checkbox(TX("Fullscreen"), &cfg_.fullscreen)) {
                SDL_SetWindowFullscreen(window_, cfg_.fullscreen);
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Hide the mouse during play"), &cfg_.hide_cursor)) cfg_.save();
            ImGui::SetItemTooltip(TX("No mouse pointer during the race. It comes back when this launcher\n"
                                     "opens (Esc) and hides again with Resume."));
            {   // exclusive fullscreen: the display's own modes, a custom 57.52 Hz one among them
                int count = 0;
                SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count);
                std::string preview = TX("Desktop (borderless)");
                for (int i = 0; i < count; ++i)
                    if (mode_key(*modes[i]) == cfg_.fullscreen_mode) preview = mode_label(*modes[i]);
                auto choose = [&](const std::string &key) {
                    cfg_.fullscreen_mode = key;
                    apply_fullscreen_mode(window_, key);
                    cfg_.save();
                };
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo(TX("Fullscreen mode"), preview.c_str())) {
                    if (ImGui::Selectable(TX("Desktop (borderless)"), cfg_.fullscreen_mode.empty())) choose("");
                    for (int i = 0; i < count; ++i) {
                        const std::string key = mode_key(*modes[i]);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(mode_label(*modes[i]).c_str(), key == cfg_.fullscreen_mode)) choose(key);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip(TX("Desktop: fullscreen at the desktop's resolution and refresh rate.\n"
                                         "A mode: exclusive fullscreen at that resolution and rate, for example\n"
                                         "a 57.52 Hz mode created in the graphics driver's settings."));
                SDL_free(modes);
            }
            ImGui::TextUnformatted(TX("Display pacing"));
            ImGui::SameLine();
            ImGui::TextDisabled(TX("(all unchecked: the cabinet's speed on any display)"));
            if (ImGui::Checkbox(TX("Smooth pacing on a 57.52 Hz display"), &cfg_.pace_smooth)) cfg_.save();
            ImGui::SetItemTooltip(TX("On a display set to 57.52 Hz (or 115.05 Hz), one game frame per refresh (or two):\n"
                                     "no doubled or skipped frame. The speed stays the cabinet's, within 1 %%.\n"
                                     "No effect at other rates."));
            if (ImGui::Checkbox(TX("Lock to the display (changes the game's speed)"), &cfg_.pace_sync_display)) cfg_.save();
            ImGui::SetItemTooltip(TX("Runs the game at a rate that divides the display's exactly: each frame\n"
                                     "stays on screen the same time and motion is perfectly smooth. On a\n"
                                     "60, 120, 180 or 240 Hz display, the game runs at 60 frames/s, about 4 %% faster than the cabinet\n"
                                     "(57.52); with the reference sound, the sound is a little faster too. No effect\n"
                                     "on a 144 or 165 Hz display, which cannot lock a rate close to the cabinet's:\n"
                                     "use VRR pacing then if the display supports G-Sync or FreeSync.\n"
                                     "Unchecked: the game always runs at the cabinet's speed."));
            if (ImGui::Checkbox(TX("VRR pacing (G-Sync / FreeSync)"), &cfg_.pace_vrr)) cfg_.save();
            ImGui::SetItemTooltip(TX("For a variable refresh display: each frame lasts exactly 1/57.52 s, the display\n"
                                     "refreshes at the game's pace and each frame stays on screen the same time.\n"
                                     "VRR must be on in the display and the graphics driver; on a fixed\n"
                                     "rate display, the result looks like the default setting."));
            ImGui::TextDisabled(TX("Currently: %s."), pacing_status_.c_str());
            if (ImGui::Checkbox(TX("Skip the launcher"), &cfg_.skip_launcher)) cfg_.save();
            ImGui::SameLine();
            ImGui::TextDisabled(TX("(starts the game right away; Esc opens this launcher)"));
            const char *draw_modes[] = {TX("Double buffer"), TX("Single buffer"), TX("One frame in three")};
            ImGui::SetNextItemWidth(200);
            int dm = std::clamp(cfg_.draw_mode, 0, 2);
            if (ImGui::Combo(TX("Draw mode"), &dm, draw_modes, 3)) {
                cfg_.draw_mode = dm;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Double buffer: every frame is drawn, as in the game. Single buffer: one in two;\n"
                                   "one frame in three: one in three. Faster; the game itself is not slowed down."));
            const char *supersampling[] = {TX("Native 2x"), TX("Disabled"), "2x", "3x", "4x", TX("Native direct")};
            ImGui::BeginDisabled(cfg_.renderer != "hardware");
            ImGui::SetNextItemWidth(200);
            int ss = std::clamp(cfg_.supersampling, 0, app::Config::kNativeDirect);
            if (ImGui::Combo(TX("Render resolution"), &ss, supersampling, IM_ARRAYSIZE(supersampling))) {
                cfg_.supersampling = ss;
                cfg_.save();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("The native modes follow the window under Direct3D 12.\n"
                                   "2x to 4x enlarge the original resolution."));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Enhancements"));
            ImGui::BeginDisabled(cfg_.arcade_43());
            if (ImGui::Checkbox(TX("Gauges at the screen edges (experimental)"), &cfg_.hud_edges)) cfg_.save();
            if (ImGui::Checkbox(TX("Stretch the sky backdrop (experimental)"), &cfg_.stretch_backdrop)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("Shows more of the scene at the sides. The gauges stay in 4:3 in the centre, or else\n"
                                   "lap times, position and maps move to the edges. In the race, the sky\n"
                                   "at the sides is plain blue, or else the game's sky picture stretched across the screen."));
            const char *distances[] = {TX("Shortest"), TX("Shorter"), TX("Default"), TX("Farther"), TX("Farthest")};
            ImGui::SetNextItemWidth(200);
            int dd = std::clamp(cfg_.draw_distance, -2, 2);
            if (ImGui::SliderInt(TX("Draw distance"), &dd, -2, 2, distances[dd + 2], ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.draw_distance = dd;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("The scenery around the track. Widescreen also adds the scenery at the sides.\n"
                                   "Shorter reduces the range; farther adds trees and buildings, not the road."));
            if (ImGui::Checkbox(TX("3D scenery"), &cfg_.scenery_3d)) cfg_.save();
            ImGui::TextDisabled(TX("Fir trees in 3D, full grandstands, pillars under the Dinosaur Canyon bridge.\n"
                                   "Applies the next time the game starts."));
            if (ImGui::Checkbox(TX("Opponent cars always detailed"), &cfg_.cars_near_model)) cfg_.save();
            ImGui::TextDisabled(TX("One model, the most detailed, at every distance: no more cars\n"
                                   "changing look as they come closer. Applies the next time the game starts."));
            ImGui::BeginDisabled(cfg_.renderer != "hardware" || cfg_.draw_mode != 0);
#ifdef M2_RAY_TRACING
            if (ImGui::Checkbox(TX("Sun shadows (ray tracing, experimental)"), &cfg_.sun_shadows)) cfg_.save();
            ImGui::TextDisabled(TX("Shadows cast by the game's sun (Direct3D 12, ray tracing capable card,\n"
                                   "native resolution). Applies the next time the game starts."));
            if (cfg_.sun_shadows) {
                if (ImGui::Checkbox(TX("Sky light (experimental)"), &cfg_.sky_light)) cfg_.save();
                ImGui::TextDisabled(TX("The sky lights from everywhere: darker under the cars, under the gantry,\n"
                                       "at the foot of walls. Applies the next time the game starts."));
                ImGui::SetNextItemWidth(200);
                int tens = cfg_.sun_height / 10;
                char sun_label[32];
                std::snprintf(sun_label, sizeof sun_label, tens ? TX("%d degrees") : TX("the game's own"), tens * 10);
                if (ImGui::SliderInt(TX("Sun height"), &tens, 0, 8, sun_label, ImGuiSliderFlags_AlwaysClamp)) {
                    cfg_.sun_height = tens * 10;
                    cfg_.save();
                }
                ImGui::TextDisabled(TX("The direction stays the game's sun's. Lower, the shadows get longer\n"
                                       "(as in late afternoon). Applies the next time the game starts."));
            }
#endif
            if (ImGui::Checkbox(TX("120 frames/s smoothing (experimental)"), &cfg_.interpolate)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("In-between frames between the game's. Needs the Hardware renderer and the double\n"
                                   "buffer.\n"
                                   "Applies the next time the game starts."));
            ImGui::SetNextItemWidth(200);
            int crt = cfg_.display_gamma >= 2.0f ? int(cfg_.display_gamma * 10.0f + 0.5f) : 20;
            char crt_label[32];
            std::snprintf(crt_label, sizeof crt_label, crt <= 20 ? TX("PC (original)") : TX("Cabinet %d.%d"), crt / 10, crt % 10);
            if (ImGui::SliderInt(TX("Screen colours (restart the game)"), &crt, 20, 26, crt_label,
                                 ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.display_gamma = crt <= 20 ? 0.0f : float(crt) / 10.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("PC: the game's colours as before (original picture).\n"
                                   "Cabinet 2.2 to 2.6: the same colours as on an arcade cabinet's screen,\n"
                                   "deeper and less washed out. Recommended: 2.4."));
            ImGui::SetNextItemWidth(200);
            int saturation = int(cfg_.display_saturation * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Colour saturation (restart the game)"), &saturation, 50, 150,
                                 saturation == 100 ? TX("100 %% (original)") : "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.display_saturation = float(saturation) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Above 100 %%: livelier colours. 100 %%: the game's colours as they are."));
            const int scenery_width = 496 + 2 * rt::GameLoop::wide_margin(cfg_.arcade_43() ? 0.0 : cfg_.aspect_ratio());
            const uint32_t automatic_budget = rt::automatic_scenery_budget(scenery_width, cfg_.draw_distance);
            int budget_mode = cfg_.draw_budget ? 1 : 0;
            const char *budget_modes[] = {TX("Automatic"), TX("Custom")};
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("Polygon budget"), &budget_mode, budget_modes, 2)) {
                cfg_.draw_budget = budget_mode ? automatic_budget : 0;
                cfg_.save();
            }
            if (cfg_.draw_budget) {
                int value = int(cfg_.draw_budget);
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputInt(TX("Custom allowance"), &value, 500, 5000)) {
                    cfg_.draw_budget = uint32_t(std::clamp(value, 1, int(rt::kMaxSceneryBudget)));
                    cfg_.save();
                }
                ImGui::TextDisabled(TX("Replaces the automatic allowance. From 1 to 1,000,000.\n"
                                       "Too low, scenery may be missing; too high, the game may slow down."));
            } else {
                ImGui::TextDisabled(TX("Current allowance: %u. It follows widescreen and the draw distance.\n"
                                       "Resolution and supersampling do not change it."), automatic_budget);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Link play (experimental, reset)"));
            if (ImGui::Checkbox(TX("Link to other cabinets"), &cfg_.link)) cfg_.save();
            ImGui::BeginDisabled(!cfg_.link);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputInt(TX("This cabinet's port"), &cfg_.link_port, 0, 0)) {
                cfg_.link_port = std::clamp(cfg_.link_port, 1, 65535);
                cfg_.save();
            }
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText(TX("Next cabinet (host:port)"), link_next_buf_, sizeof link_next_buf_)) {
                cfg_.link_next = link_next_buf_;
                cfg_.save();
            }
            if (ImGui::Checkbox(TX("Synchronise frames (each cabinet waits for the master)"), &cfg_.link_framesync)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640);
            ImGui::TextWrapped(TX("Network: %s"), link_status_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::TextDisabled(TX("The cabinets link in a ring over the network (Wi-Fi or cable): each listens on its\n"
                                   "port and connects to the next one; with two, each has the other as next. All the\n"
                                   "computers can take the same port, for example 15112, and Next cabinet =\n"
                                   "the other computer's address:15112. In test mode (F2) > GAME SYSTEM, set LINK ID\n"
                                   "(one MASTER, the others SLAVE) and a different CAR NUMBER on each. The 1994 game\n"
                                   "(revision A) supports link play."));

            ImGui::Spacing();
            ImGui::Separator();
            if (!error_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", error_.c_str());
                ImGui::PopStyleColor();
            }
            if (!from_home) {
                ImGui::Checkbox(TX("Hold the Test button"), &cfg_.hold_test); // not saved: one press, then it clears
#ifdef M2_MOBILE
                ImGui::TextWrapped(TX("Opens the test menu: coin settings, game type; without an F2 key."));
#else
                ImGui::SameLine();
                ImGui::TextDisabled(TX("(opens the test menu: coin settings, game type; without an F2 key)"));
#endif
                ImGui::SetItemTooltip(TX("Check, then Start or Resume: once the game runs, the Test button is held\n"
                                         "for 3 seconds, which opens the test menu, then the box unchecks itself.\n"
                                         "F2, or any button assigned to Test, still works."));
                ImGui::BeginDisabled(!rom_ok_);
                if (game_running) {
                    if (ImGui::Button(TX("Resume"), ImVec2(140, 40))) result = Resume;
                    ImGui::SameLine();
                    if (ImGui::Button(TX("Reset"), ImVec2(140, 40))) result = Reset;
                } else if (ImGui::Button(TX("Start game"), ImVec2(140, 40))) {
                    result = StartGame;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(TX("Quit"), ImVec2(140, 40))) result = Quit;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(TX("Controls"))) {
            Controls &c = cfg_.controls;
            ImGui::Spacing();
            ImGui::Text(TX("Gamepad: %s"), pad ? SDL_GetGamepadName(pad) : TX("none connected"));
            std::string joys;
            for (SDL_Joystick *j : devices.joys) {
                const char *n = SDL_GetJoystickName(j);
                joys += (joys.empty() ? "" : ", ") + std::string(n ? n : "joystick");
            }
            ImGui::TextWrapped(TX("Wheels and joysticks: %s"), joys.empty() ? TX("none found") : joys.c_str());
            // live meters
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const float steer = c.value(SteerRight, keys, devices) - c.value(SteerLeft, keys, devices);
            char label[32];
            std::snprintf(label, sizeof label, "%+.2f", c.steer_invert ? -steer : steer);
            ImGui::ProgressBar(0.5f + 0.5f * (c.steer_invert ? -steer : steer), ImVec2(200, 0), label);
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Steering"));
            const float acc = c.value(Accelerate, keys, devices), brk = c.value(Brake, keys, devices);
            ImGui::ProgressBar(acc, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Accelerator"));
            ImGui::ProgressBar(brk, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Brake pedal"));
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat(TX("Dead zone"), &c.deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Invert steering"), &c.steer_invert)) cfg_.save();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat(TX("Wheel dead zone (experimental)"), &c.joy_deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SetNextItemWidth(200);
            int wheel_sensitivity = int(c.wheel_sensitivity * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Wheel sensitivity"), &wheel_sensitivity, 25, 300, "%d%%")) {
                c.wheel_sensitivity = float(wheel_sensitivity) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("100 %%: linear. Lower: gentler near the centre. Higher: sharper.\n"
                                   "Only for the steering axis of a wheel or joystick; full lock is kept."));
            ImGui::SetNextItemWidth(200);
            int pad_sensitivity = int(c.pad_steer_sensitivity * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Stick sensitivity (gamepad)"), &pad_sensitivity, 25, 100, "%d%%")) {
                c.pad_steer_sensitivity = float(pad_sensitivity) / 100.0f;
                cfg_.save();
            }
            ImGui::SetNextItemWidth(200);
            int pad_speed = int(c.pad_steer_speed * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Steering speed (gamepad)"), &pad_speed, 2, 100, pad_speed >= 100 ? TX("Immediate") : "%d%%")) {
                c.pad_steer_speed = float(pad_speed) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Gamepad stick only. Sensitivity: lower, gentler near the centre (full lock kept).\n"
                                   "Speed: how fast the game's wheel follows the stick, per frame (100 %%: at once)."));
            ImGui::TextDisabled(TX("Triggers, sticks, wheels and pedals are analogue. To assign a wheel or pedal axis,\n"
                                   "click its button, then turn the wheel or press the pedal as far as you want\n"
                                   "full lock or full travel, and let go: that sets its range."));
#ifndef _WIN32 // SDL's own Logitech driver is off on Windows already
            if (ImGui::Checkbox(TX("Legacy Logitech wheel support (restart the game)"), &cfg_.legacy_logitech_wheels)) cfg_.save();
            ImGui::TextDisabled(TX("Logitech wheels through the system driver: needed for a wheel that is listed but does\n"
                                   "nothing when assigned (the original Driving Force). Unchecked: SDL's driver, which gives\n"
                                   "force feedback to recent wheels under macOS. After a change, restart and\n"
                                   "assign the wheel's controls again."));
#endif

            ImGui::SetNextItemWidth(200);
            int ffb = int(cfg_.ffb_strength * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Force feedback (experimental)"), &ffb, 0, 100, ffb ? "%d%%" : TX("Off"))) {
                cfg_.ffb_strength = float(ffb) / 100.0f;
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Invert the force"), &cfg_.ffb_invert)) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Force feedback log"), &cfg_.ffb_log)) cfg_.save();
#ifdef __linux__ // for Linux drivers without a spring effect (the kernel's Logitech driver)
            if (ImGui::Checkbox(TX("Centring computed by the game"), &cfg_.ffb_centring_override)) cfg_.save();
            ImGui::SetItemTooltip(TX("For a wheel that kicks on impacts but does not recentre in bends: its\n"
                                     "Linux driver has no spring effect (the log's first line says \"spring no\").\n"
                                     "The game's recentring is then computed here from the wheel's position and\n"
                                     "sent as a constant force. Leave unchecked if the wheel already recentres."));
#endif
            ImGui::TextDisabled(TX("The cabinet wheel's motor (recentring, resistance, wheel pulling), on the device\n"
                                   "assigned to steering; a gamepad rumbles when the car is pushed. Currently: %s.\n"
                                   "Wheels and force feedback are not yet tested on real hardware: feedback welcome.\n"
                                   "The log goes to daytona.log, next to launcher.ini under Windows, else to the terminal."), ffb_device_);

            if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn(TX("Control"), ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn(TX("Keyboard"), ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn(TX("Gamepad"), ImGuiTableColumnFlags_WidthFixed, 250);
                ImGui::TableSetupColumn(TX("Wheel / joystick (experimental)"));
                ImGui::TableHeadersRow();
                for (int a = 0; a < kNumActions; a++) {
                    ImGui::PushID(a);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action_name(Action(a)));
                    ImGui::TableNextColumn();
                    const bool cap_key = capture_action_ == a && capture_kind_ == CaptureKey;
                    const char *kn = c.bind[a].key == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(c.bind[a].key);
                    if (ImGui::Button(cap_key ? TX("Press a key...") : kn, ImVec2(120, 0))) start_capture(a, CaptureKey, devices);
                    ImGui::TableNextColumn();
                    const bool cap_pad = capture_action_ == a && capture_kind_ == CapturePad;
                    const std::string pn = cap_pad ? TX("Press or move...") : c.bind[a].pad.describe();
                    if (ImGui::Button(pn.c_str(), ImVec2(190, 0))) start_capture(a, CapturePad, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TX("clear"))) {
                        c.bind[a].pad = PadInput{};
                        cfg_.save();
                    }
                    ImGui::TableNextColumn();
                    const bool cap_joy = capture_action_ == a && capture_kind_ == CaptureJoy;
                    const std::string jn = !cap_joy ? c.bind[a].joy.describe(devices)
                                           : joy_tracking_ ? TX("As far as wanted, then let go...")
                                                           : TX("Press, or move all the way and let go...");
                    if (ImGui::Button(jn.c_str(), ImVec2(300, 0))) start_capture(a, CaptureJoy, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TX("clear##joy"))) {
                        c.bind[a].joy = JoyInput{};
                        cfg_.save();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled(TX("Esc cancels an assignment; Backspace clears a key."));
            if (ImGui::Button(TX("Reset the default settings"))) {
                c.set_defaults();
                cfg_.save();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TX("Sound"))) {
            ImGui::Spacing();
            auto percent = [&](const char *label, float &value) { // a 0..1 setting as 0-100%
                ImGui::SetNextItemWidth(200);
                int v = int(value * 100.0f + 0.5f);
                if (ImGui::SliderInt(label, &v, 0, 100, "%d%%")) {
                    value = float(v) / 100.0f;
                    cfg_.save();
                }
            };
            percent(TX("Volume"), cfg_.volume);
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Mute"), &cfg_.mute)) cfg_.save();
            ImGui::Spacing();
            percent(TX("Music"), cfg_.music_volume);
            percent(TX("Effects"), cfg_.effects_volume);
            ImGui::TextDisabled(TX("The balance between the music and everything else (engine, skids, impacts).\n"
                                   "100 %% and 100 %%: the cabinet sound board's mix."));

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox(TX("Native sound (experimental, reset)"), &cfg_.native_audio)) cfg_.save();
            ImGui::TextDisabled(TX("Native sequencer and mixer; the reference sound stays available for comparison."));
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopTextWrapPos();
#ifdef M2_MOBILE
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(0))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
#endif
    ImGui::End();
    return result;
}

} // namespace app
