#include "app/launcher.h"
#include "app_version.h" // generated: cmake/app_version.cmake
#include "app/i18n.h"
#include "app/gpu/quality_probe.h"
#include "app/neon_ui.h"
#include "app/rom_file.h"
#ifdef SDL_PLATFORM_IOS
#include "rom_picker.h"
#endif
#include "runtime/game_loop.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
#ifndef M2_MOBILE
    if (cfg_.rom_path.empty()) {
        // none chosen yet: the project's roms/ folder, where the build took it from
        // (build-daytona/ is beside roms/), or roms/ in the working folder
        const char *base = SDL_GetBasePath();
        for (const std::string &dir : {std::string(base ? base : "") + "../roms/", std::string("roms/")})
            for (const char *ext : {".zip", ".7z"}) {
                std::error_code ec;
                const std::string candidate = dir + M2_ROMSET + ext;
                if (cfg_.rom_path.empty() && std::filesystem::exists(candidate, ec)) cfg_.rom_path = candidate;
            }
        if (!cfg_.rom_path.empty()) std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
    }
#endif
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

namespace {
// The settings screen's pieces, in the Online screen's style: a label above
// a full-width field, help text wrapped under it.
void hint(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

bool combo(const char *label, const char *id, int *value, const char *const items[], int count) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::Combo(id, value, items, count);
}

bool slider(const char *label, const char *id, int *value, int lo, int hi, const char *format) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
}

// Two blocks side by side on a wide window, one under the other otherwise.
template <class Left, class Right> void columns(const char *id, bool wide, Left left, Right right) {
    if (wide && ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableNextColumn();
        left();
        ImGui::TableNextColumn();
        right();
        ImGui::EndTable();
    } else {
        left();
        ImGui::Spacing();
        right();
    }
}
} // namespace

void Launcher::take_dialog_result() {
    std::lock_guard<std::mutex> g(dialog_mutex_);
    if (!dialog_result_.empty()) {
        cfg_.rom_path = dialog_result_;
        dialog_result_.clear();
        std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
        cfg_.save();
        check_rom();
    }
}

// The interface language, each in its own script (app/i18n.h); only those with a translation.
bool Launcher::language_combo(float width) {
    const auto &langs = i18n::languages();
    size_t sel = 0;
    for (size_t i = 0; i < langs.size(); ++i)
        if (i18n::current() == langs[i].code) sel = i;
    bool changed = false;
    ImGui::TextDisabled("%s", TX("Language"));
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##language", langs[sel].name)) {
        for (size_t i = 0; i < langs.size(); ++i)
            if (i18n::available(langs[i].code) && ImGui::Selectable(langs[i].name, i == sel) && i != sel) {
                cfg_.language = langs[i].code;
                cfg_.save();
                i18n::set(cfg_.language);
                check_rom(); // its message, in the new language
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

// diagnostic: M2_CAPTURE_ROM_GATE=1 shows the ROM set screen (with a sample refusal) in a --capture-home image
bool Launcher::capture_gate() {
    static const bool on = [] {
        const char *v = std::getenv("M2_CAPTURE_ROM_GATE");
        return v && *v == '1';
    }();
    return on;
}

// The PC home's first screen until a ROM set is verified: choose it (the file
// window, or its path typed), its files checked as it is chosen.
Launcher::QualityChoice Launcher::draw_quality(const QualityProbe &probe, float shown, bool game_running) {
    using Stage = QualityProbe::Stage;
    QualityChoice choice = QualityChoice::None;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.7f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const float width = std::min(view->Size.x - 32.f, 800.f * scale);
    if (!ImGui::IsPopupOpen("##quality")) ImGui::OpenPopup("##quality");
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, view->Size.y - 32.f});
    ImGui::SetNextWindowPos({view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f}, ImGuiCond_Always,
                            {0.5f, 0.5f});
    ImGui::PushFont(nullptr, 19.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.f * scale, 18.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * scale, 7.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.f * scale, 7.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    if (ImGui::BeginPopupModal("##quality", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        const float full = ImGui::GetContentRegionAvail().x, button_h = 44.f * scale;
        ImGui::PushFont(nullptr, 30.f * scale);
        ImGui::TextUnformatted(TX("Image quality"));
        ImGui::PopFont();
        ImGui::TextDisabled("%s", probe.gpu_name().empty() ? TX("Graphics card") : probe.gpu_name().c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        const char *levels[] = {TX("Original"), TX("Medium"), TX("High"), TX("Maximum")};
        if (probe.stage() == Stage::Failed) {
            ImGui::TextUnformatted(TX("The graphics card could not be measured. Your settings stay as they are."));
            ImGui::TextDisabled("%s", probe.error().c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(TX("OK"), {full, button_h})) choice = QualityChoice::Keep;
        } else if (probe.running() || shown < 1.f) {
            quality_pick_ = -1;
            ImGui::TextUnformatted(TX("Measuring what your graphics card can draw, to recommend the best image for it."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::ProgressBar(shown, {full, 0}, "");
            // the levels are measured in this order; the bar is paced to be read
            ImGui::TextDisabled("%s", shown < 0.1f ? TX("Preparing the test...")
                                    : shown < 0.4f ? TX("Testing the maximum quality...")
                                    : shown < 0.7f ? TX("Testing the high quality...")
                                                   : TX("Testing the medium quality..."));
        } else {
            const QualityLevel recommended = probe.level();
            if (quality_pick_ < 0) quality_pick_ = int(recommended);
            char line[256];
            std::snprintf(line, sizeof line, TX("Recommended: %s"), levels[int(recommended)]);
            ImGui::PushFont(nullptr, 25.f * scale);
            ImGui::TextColored({0.48f, 0.98f, 0.79f, 1}, "%s", line);
            ImGui::PopFont();
            ImGui::TextUnformatted(TX("Pick a level, or keep your settings. Settings > Enhancements change each option later."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            const QualityTimes &t = probe.times();
            const char *what[] = {
                TX("The game's original image, without the enhancements."),
                TX("Twice the arcade's resolution, antialiasing 2x, anisotropic filtering 4x, smooth car shading and paint reflections."),
                TX("4 times the arcade's resolution, antialiasing 4x, anisotropic filtering 16x and every car option."),
                TX("The screen's own resolution, antialiasing 4x, anisotropic filtering 16x and every car option.")};
            const double ms[] = {-1, t.medium, t.high, t.maximum};
            const Stage stages[] = {Stage::Idle, Stage::Medium, Stage::High, Stage::Maximum};
            for (int k = 3; k >= 0; --k) {
                std::snprintf(line, sizeof line, "%s%s", levels[k], k == int(recommended) ? TX(" (recommended)") : "");
                if (ImGui::RadioButton(line, quality_pick_ == k)) quality_pick_ = k;
                if (ms[k] >= 0) {
                    const auto size = probe.size_of(stages[k]);
                    std::snprintf(line, sizeof line, TX("Measured: %.1f ms a frame at %d x %d"), ms[k], size.w, size.h);
                    ImGui::SameLine();
                    ImGui::TextColored(ms[k] <= kQualityBudgetMs ? ImVec4(0.48f, 0.98f, 0.79f, 1) : ImVec4(1, 0.74f, 0.45f, 1),
                                       "%s", line);
                }
                ImGui::Indent(32.f * scale);
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", what[k]);
                ImGui::PopTextWrapPos();
                ImGui::Unindent(32.f * scale);
            }
            ImGui::PushTextWrapPos(0);
            std::snprintf(line, sizeof line, TX("A level is recommended when it takes under %.0f ms. Bloom and the CRT filter are not changed."),
                          kQualityBudgetMs);
            ImGui::TextDisabled("%s", line);
            if (game_running) ImGui::TextDisabled("%s", TX("Antialiasing applies the next time the game starts."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(TX("Apply"), {half, button_h})) choice = QualityChoice::Apply;
            ImGui::SameLine();
            if (ImGui::Button(TX("Keep my settings"), {half, button_h})) choice = QualityChoice::Keep;
        }
        if (choice != QualityChoice::None) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
    return choice;
}

Launcher::Result Launcher::draw_rom_gate() {
    take_dialog_result();
    if (capture_gate() && !gate_open_) {
        gate_open_ = true;
        rom_ok_ = false;
        rom_message_ = "2 files of 30 are missing or wrong (a sample, for the capture)."; // diagnostic only
    }
    Result result = Stay;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.7f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const float width = std::min(view->Size.x - 32.f, 760.f * scale);
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, view->Size.y - 32.f});
    ImGui::SetNextWindowPos({view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f}, ImGuiCond_Always,
                            {0.5f, 0.5f});
    ImGui::PushFont(nullptr, 21.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.f * scale, 20.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::Begin("##rom_gate", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const ImVec4 good(0.48f, 0.98f, 0.79f, 1), warn(1, 0.74f, 0.45f, 1);
    const float full = ImGui::GetContentRegionAvail().x, button_h = 52.f * scale;
    {
        const float lang_w = 200.f * scale;
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, 30.f * scale);
        ImGui::TextUnformatted(TX("Your ROM set"));
        ImGui::PopFont();
        ImGui::TextDisabled("v%s", DAYTONA_APP_VERSION);
        ImGui::EndGroup();
        ImGui::SameLine(full - lang_w + ImGui::GetStyle().WindowPadding.x);
        ImGui::BeginGroup();
        language_combo(lang_w);
        ImGui::EndGroup();
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    char what[256];
    std::snprintf(what, sizeof what,
                  TX("The game is rebuilt from your own Daytona USA ROM set: MAME's %s set, a .zip or .7z. "
                     "Choose it to start; nothing is sent anywhere."),
                  M2_ROMSET);
    ImGui::TextUnformatted(what);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button(dialog_pending_ ? TX("Choosing...") : TX("Choose the ROM set..."), {full, button_h}) &&
        !dialog_pending_)
        browse();
    ImGui::TextDisabled("%s", TX("Or type its full path and press Enter:"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##rom_gate_path", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        cfg_.rom_path = path_buf_;
        cfg_.save();
        check_rom();
    }
    if (!rom_message_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(rom_ok_ ? good : warn, "%s", rom_message_.c_str());
        ImGui::PopTextWrapPos();
    }
    // refused: only the files that are missing or wrong
    const bool any_bad = std::any_of(checks_.begin(), checks_.end(), [](const auto &c) { return !c.ok; });
    if (!rom_ok_ && any_bad && ImGui::BeginTable("##gate_files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto &c : checks_) {
            if (c.ok) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.file.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(warn, "%s", c.problem.c_str());
        }
        ImGui::EndTable();
    }
    if (gate_open_) { // opened from Settings: the verified set stays if nothing else is chosen
        ImGui::Spacing();
        ImGui::BeginDisabled(!rom_ok_);
        if (ImGui::Button(TX("Done"), {full, button_h * 0.85f})) {
            gate_open_ = false;
            result = Back;
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
    return result;
}

#ifndef M2_MOBILE
// Settings > AI textures (app/hires_maker.h, docs/HIRES-TEXTURES.md): what
// this computer has, making the pack with its progress, and using it.
void Launcher::draw_hires_tab(float scale) {
    const ImVec4 good(0.48f, 0.98f, 0.79f, 1), warn(1, 0.74f, 0.45f, 1), bad(1, 0.45f, 0.4f, 1);
    using Stage = HiresMaker::Stage;
    using Problem = HiresMaker::Problem;
    HiresMaker::State st = hires_.state();
    if (!st.req.checked && !hires_.busy()) hires_.check(), st = hires_.state();
    const bool busy = hires_.busy();
    // a pack just made is used at once
    static uint64_t enabled_for = 0;
    if (st.stage == Stage::Done && st.pack_version != enabled_for) {
        enabled_for = st.pack_version;
        cfg_.hires_textures = true;
        cfg_.save();
    }
    ImGui::Spacing();
    ImGui::TextColored(warn, "%s", TX("Experimental: this feature is being tested. It may fail or change; "
                                      "if it does, the log below helps us fix it."));
    ImGui::SeparatorText(TX("High-resolution textures"));
    hint(TX("The race's textures upscaled four times by AI models, on this computer, from your own ROM set. "
            "The pack stays on this computer: it is made of the game's textures and is never shared."));

    ImGui::Spacing();
    ImGui::SeparatorText(TX("This computer"));
    const HiresMaker::Requirements &r = st.req;
    if (!r.checked) {
        ImGui::TextDisabled("%s", TX("Checking..."));
    } else {
        auto line = [&](bool ok, const char *text) { ImGui::TextColored(ok ? good : warn, "%s %s", ok ? "OK" : "--", text); };
        if (r.nvidia) {
            char gpu[256];
            std::snprintf(gpu, sizeof gpu, TX("Graphics card: %s"), r.gpu.c_str());
            line(true, gpu);
        } else {
            line(false, TX("No NVIDIA graphics card found: the processor can make the pack, slowly."));
        }
        if (!r.supported) {
            line(false, TX("Making the pack works on Linux for now."));
        } else {
            line(r.docker, r.docker ? TX("Docker: ready")
                           : r.docker_installed ? TX("Docker: installed, but this user cannot use it (add it to the docker group)")
                                                : TX("Docker: not installed"));
            if (r.docker && r.nvidia)
                line(r.docker_gpu, r.docker_gpu ? TX("NVIDIA Container Toolkit: ready")
                                                : TX("NVIDIA Container Toolkit: missing (Docker cannot use the graphics card)"));
        }
        if (!r.kit) line(false, TX("The maker's files are missing next to the game: build the game again."));
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(TX("Check again"))) hires_.check();
        ImGui::EndDisabled();
    }
    hint(TX("What is needed, and why: docs/HIRES-TEXTURES.md."));

    ImGui::Spacing();
    ImGui::SeparatorText(TX("Make the pack"));
    const bool card = r.nvidia && r.docker_gpu;
    const bool can = r.checked && r.supported && r.kit && r.docker && (card || cfg_.hires_processor) && rom_ok_;
    ImGui::BeginDisabled(busy);
    if (ImGui::Checkbox(TX("On the processor (slow: about 30 minutes)"), &cfg_.hires_processor)) cfg_.save();
    ImGui::EndDisabled();
    hint(TX("Without a graphics card the tools run on the processor: about 30 minutes on 20 threads (estimated from a sample)."));
    const bool working = st.stage == Stage::Capturing || st.stage == Stage::Building || st.stage == Stage::Upscaling ||
                         st.stage == Stage::Installing || st.stage == Stage::Cleaning;
    if (working) {
        const char *what = st.stage == Stage::Capturing ? TX("Capturing the game's textures...")
                           : st.stage == Stage::Building ? TX("Preparing the tools (the first time: a download of several GB)...")
                           : st.stage == Stage::Upscaling ? TX("Upscaling the textures...")
                           : st.stage == Stage::Installing ? TX("Installing the pack...")
                                                           : TX("Removing the tools...");
        ImGui::TextUnformatted(what);
        ImGui::ProgressBar(st.progress, {-1, 0});
        if (ImGui::Button(TX("Cancel"))) hires_.cancel();
    } else {
        ImGui::BeginDisabled(!can || busy);
        if (ImGui::Button(TX("Make the pack"), {260 * scale, 0})) {
            hires_.start(cfg_.rom_path, cfg_.hires_processor || !card);
        }
        ImGui::EndDisabled();
        hint(TX("About 2 minutes with the graphics card, after the tools' download. The tools are removed once the pack is made."));
    }
    if (st.stage == Stage::Done) ImGui::TextColored(good, "%s", TX("The pack is ready."));
    if (st.stage == Stage::Cancelled) ImGui::TextColored(warn, "%s", TX("Cancelled."));
    if (st.stage == Stage::Failed) {
        const char *why = "";
        switch (st.problem) {
        case Problem::Unsupported: why = TX("This system cannot make the pack yet."); break;
        case Problem::Rom: why = TX("The ROM set was not found (Game tab)."); break;
        case Problem::Kit: why = TX("The maker's files are missing next to the game: build the game again."); break;
        case Problem::DockerMissing: why = TX("Docker is not installed."); break;
        case Problem::DockerAccess: why = TX("This user cannot use Docker (add it to the docker group)."); break;
        case Problem::NoNvidia: why = TX("No NVIDIA graphics card answered."); break;
        case Problem::NoToolkit: why = TX("Docker cannot use the graphics card: install the NVIDIA Container Toolkit."); break;
        case Problem::Capture: why = TX("The capture of the game's textures failed."); break;
        case Problem::Build: why = TX("The upscaling tools could not be prepared."); break;
        case Problem::Upscale: why = TX("The upscaling failed."); break;
        case Problem::BadPack: why = TX("The pack made is not valid."); break;
        case Problem::Save: why = TX("The pack could not be saved."); break;
        case Problem::None: break;
        }
        ImGui::TextColored(bad, "%s", why);
        char log[1200];
        std::snprintf(log, sizeof log, TX("Details in %s"), HiresMaker::log_path().c_str());
        ImGui::TextWrapped("%s", log);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TX("The pack"));
    std::error_code ec;
    const auto size = std::filesystem::file_size(HiresMaker::pack_path(), ec);
    if (ec) {
        ImGui::TextDisabled("%s", TX("No pack yet."));
    } else {
        char info[200];
        std::snprintf(info, sizeof info, TX("%.0f MB, on this computer"), double(size) / 1e6);
        ImGui::TextUnformatted(info);
        if (ImGui::Checkbox(TX("Use the high-resolution textures"), &cfg_.hires_textures)) cfg_.save();
        hint(TX("Needs the Hardware renderer (Display tab). Textures the pack does not have are drawn as before."));
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(TX("Delete the pack"))) {
            hires_.delete_pack();
            cfg_.hires_textures = false;
            cfg_.save();
        }
        ImGui::EndDisabled();
    }
}
#endif

Launcher::Result Launcher::draw(bool game_running, const Devices &devices, bool from_home) {
    SDL_Gamepad *pad = devices.pad;
    take_dialog_result();

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
#ifdef M2_MOBILE
    const float scale = 1.0f; // the mobile style is sized for touch already (main.cpp)
    const bool neon_look = false;
#else
    const float scale = std::max(0.7f, std::min(vp->WorkSize.y / 900.f, vp->WorkSize.x / 980.f));
    // from the PC home: a neon panel over the dimmed scene, as the room's settings (app/online_menu.cpp)
    const bool neon_look = from_home && body_font_;
    int neon_colors = 0, neon_vars = 0;
    if (neon_look) {
        using namespace neon;
        const float pw = std::min(vp->WorkSize.x - 32.f, 1060.f * scale), ph = vp->WorkSize.y - 48.f * scale;
        const ImVec2 a = {vp->WorkPos.x + (vp->WorkSize.x - pw) * 0.5f, vp->WorkPos.y + 24.f * scale}, b = {a.x + pw, a.y + ph};
        auto *bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(vp->Pos, {vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y}, rgba(0.01f, 0.03f, 0.08f, game_running ? 0.75f : 0.55f));
        const auto panel = rounded(a, b, 0, 16 * scale);
        fill(bg, panel, rgba(0.05f, 0.11f, 0.25f, 0.97f), rgba(0.02f, 0.05f, 0.12f, 0.97f), a.y, b.y);
        glow(bg, panel, kBlue, 0.9f, scale);
        ImGui::SetNextWindowPos(a);
        ImGui::SetNextWindowSize({pw, ph});
        ImGui::SetNextWindowBgAlpha(0);
        const std::pair<ImGuiCol, ImVec4> colors[] = {
            {ImGuiCol_ChildBg, {0, 0, 0, 0}},
            {ImGuiCol_FrameBg, {0.01f, 0.03f, 0.08f, 0.94f}},
            {ImGuiCol_FrameBgHovered, {0.05f, 0.12f, 0.26f, 0.95f}},
            {ImGuiCol_FrameBgActive, {0.07f, 0.18f, 0.38f, 1}},
            {ImGuiCol_Border, {0.38f, 0.78f, 1, 0.55f}},
            {ImGuiCol_Button, {0.16f, 0.44f, 0.92f, 1}},
            {ImGuiCol_ButtonHovered, {0.30f, 0.64f, 1, 1}},
            {ImGuiCol_ButtonActive, {0.10f, 0.32f, 0.80f, 1}},
            {ImGuiCol_CheckMark, {0.45f, 0.86f, 1, 1}},
            {ImGuiCol_SliderGrab, {0.38f, 0.78f, 1, 1}},
            {ImGuiCol_SliderGrabActive, {0.62f, 0.90f, 1, 1}},
            {ImGuiCol_Header, {0.16f, 0.44f, 0.92f, 0.85f}},
            {ImGuiCol_HeaderHovered, {0.30f, 0.64f, 1, 0.9f}},
            {ImGuiCol_HeaderActive, {0.10f, 0.32f, 0.80f, 1}},
            {ImGuiCol_Tab, {0.06f, 0.13f, 0.28f, 1}},
            {ImGuiCol_TabHovered, {0.30f, 0.64f, 1, 1}},
            {ImGuiCol_TabSelected, {0.18f, 0.48f, 0.96f, 1}},
            {ImGuiCol_TabSelectedOverline, {0.62f, 0.90f, 1, 1}},
            {ImGuiCol_Separator, {0.38f, 0.78f, 1, 0.35f}},
            {ImGuiCol_Text, {0.97f, 0.98f, 1, 1}},
            {ImGuiCol_TextDisabled, {0.70f, 0.78f, 0.90f, 1}},
            {ImGuiCol_PopupBg, {0.04f, 0.08f, 0.18f, 0.98f}},
            {ImGuiCol_ScrollbarBg, {0, 0, 0, 0}},
            {ImGuiCol_ScrollbarGrab, {0.38f, 0.78f, 1, 0.45f}},
            {ImGuiCol_ScrollbarGrabHovered, {0.38f, 0.78f, 1, 0.7f}},
            {ImGuiCol_ScrollbarGrabActive, {0.62f, 0.90f, 1, 0.9f}},
            {ImGuiCol_NavCursor, {1, 0.9f, 0.35f, 1}},
        };
        for (const auto &[col, value] : colors) ImGui::PushStyleColor(col, value);
        neon_colors = int(std::size(colors));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {30.f * scale, 22.f * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f * scale);
        neon_vars = 4;
    }
    ImGui::PushFont(neon_look ? body_font_ : nullptr, (neon_look ? 20.f : 19.f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * scale, 7.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.f * scale, 8.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, (neon_look ? 6.f : 5.f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 24.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, (neon_look ? 6.f : 5.f) * scale);
#endif
    ImGui::Begin("Daytona USA", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    // one centred column, as wide as reads well
    const float avail = ImGui::GetContentRegionAvail().x;
    const float column = std::min(avail, 1000.f * scale);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - column) * 0.5f);
    // the neon panel: Back stays at its foot, the rest scrolls above it
    const float footer = neon_look ? 82.f * scale : 0.f;
    ImGui::BeginChild("##settings", {column, -footer});
    ImGui::PushTextWrapPos(0.0f);
    const bool wide = column >= 620.f && !neon_look; // the neon panel: one column, it scrolls
    const float button_h = 44.f * scale;
    const ImVec4 good(0.48f, 0.98f, 0.79f, 1), warn(1, 0.74f, 0.45f, 1), bad(1, 0.45f, 0.4f, 1);

    // header: back, title, language
    const float header_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
#ifndef M2_MOBILE
    if (neon_look) { // a gear in a badge, the title; the language and Back at the right
        using namespace neon;
        auto *wd = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        const ImVec2 badge = {at.x + 22 * scale, at.y + 26 * scale};
        wd->AddCircleFilled(badge, 22 * scale, rgba(0.02f, 0.06f, 0.15f, 0.95f), 32);
        wd->AddCircle(badge, 22 * scale, rgba(0.38f, 0.80f, 1), 32, 2 * scale);
        gear(wd, badge, 26 * scale, rgba(0.97f, 0.98f, 1), rgba(0.02f, 0.06f, 0.15f));
        italic(wd, display_font_ ? display_font_ : body_font_, 32 * scale, {at.x + 58 * scale, at.y + 4 * scale}, rgba(0.97f, 0.98f, 1),
               TX("Settings"), 0.18f);
        char sub[160];
        std::snprintf(sub, sizeof sub, "%s   v%s (%s)", TX("static recompilation, native engine"), DAYTONA_APP_VERSION, DAYTONA_APP_COMMIT);
        wd->AddText(body_font_, 15 * scale, {at.x + 60 * scale, at.y + 44 * scale}, rgba(0.70f, 0.78f, 0.90f), sub);
        const float lang_w = 220 * scale;
        ImGui::SetCursorScreenPos({right - lang_w, at.y - 2 * scale});
        ImGui::BeginGroup(); // its label and its list, one under the other, at the right
        language_combo(lang_w);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({at.x, at.y + 72 * scale});
        ImGui::Dummy({1, 1});
    } else
#endif
    if (from_home) {
        if (ImGui::Button(TX("Back"), {120.f * scale, button_h})) result = Back;
        ImGui::SameLine();
    }
    if (!neon_look) {
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, 30.f * scale);
        ImGui::TextUnformatted(from_home ? TX("Settings") : "DAYTONA USA");
        ImGui::PopFont();
        ImGui::TextDisabled("%s   v%s (%s)", TX("static recompilation, native engine"), DAYTONA_APP_VERSION, DAYTONA_APP_COMMIT);
        ImGui::EndGroup();
    }
    if (!neon_look) {
        const float width = 200.f * scale;
        ImGui::SameLine(header_right - width);
        ImGui::BeginGroup();
        language_combo(width);
        ImGui::EndGroup();
    }
    ImGui::Spacing();

    // diagnostic: M2_CAPTURE_SETTINGS_TAB=N opens tab N (0 Game .. 5 Sound, 6 AI textures) once, for a --capture-home image
    static int open_tab = [] {
        const char *v = std::getenv("M2_CAPTURE_SETTINGS_TAB");
        return v && *v ? std::atoi(v) : -1;
    }();
    const auto tab_flags = [&](int tab) { return tab == open_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabBar("tabs")) {
        // ---- Game: the ROM set; starting, resuming, quitting (the cabinet launcher)
        if (ImGui::BeginTabItem(TX("Game"), nullptr, tab_flags(0))) {
            ImGui::Spacing();
            ImGui::SeparatorText(TX("ROM set"));
            const float small = 140.f * scale, gap = ImGui::GetStyle().ItemSpacing.x;
#ifndef M2_MOBILE
            if (from_home) { // the PC home chose it on its first screen: its state, and Change...
                const float change_w = 160.f * scale;
                ImGui::BeginGroup();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - change_w - ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextUnformatted(cfg_.rom_path.c_str());
                ImGui::TextColored(rom_ok_ ? good : warn, "%s", rom_message_.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - change_w);
                if (ImGui::Button(TX("Change..."), {change_w, 0})) gate_open_ = true;
            } else {
#else
            {
#endif
#ifdef SDL_PLATFORM_ANDROID
            hint(TX("Browse gives access to your ZIP/7z. A verified copy is kept in the app."));
#endif
            ImGui::SetNextItemWidth(std::max(120.f * scale, ImGui::GetContentRegionAvail().x - 2 * (small + gap)));
            if (ImGui::InputText("##rom", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::SameLine();
            if (ImGui::Button(TX("Browse..."), {small, 0}) && !dialog_pending_) browse();
            ImGui::SameLine();
            if (ImGui::Button(TX("Check"), {small, 0})) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::TextColored(rom_ok_ ? good : warn, "%s", rom_message_.c_str());
            if (!checks_.empty() && ImGui::TreeNode(TX("Files"))) {
                if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const auto &c : checks_) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.file.c_str());
                        ImGui::TableNextColumn();
                        if (c.ok) ImGui::TextColored(good, "ok");
                        else ImGui::TextColored(warn, "%s", c.problem.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            } // the full ROM set controls (the cabinet launcher, mobile)
            if (!error_.empty()) ImGui::TextColored(bad, "%s", error_.c_str());
            if (!from_home) {
                ImGui::Spacing();
                ImGui::SeparatorText(TX("Play"));
                ImGui::Checkbox(TX("Hold the Test button"), &cfg_.hold_test); // not saved: one press, then it clears
                ImGui::SetItemTooltip("%s", TX("Check, then Start or Resume: once the game runs, the Test button is held\n"
                                         "for 3 seconds, which opens the test menu, then the box unchecks itself.\n"
                                         "F2, or any button assigned to Test, still works."));
                hint(TX("Opens the test menu: coin settings, game type; without an F2 key."));
                ImGui::Spacing();
                const float third = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3.f;
                ImGui::BeginDisabled(!rom_ok_);
                if (game_running) {
                    if (ImGui::Button(TX("Resume"), {third, button_h})) result = Resume;
                    ImGui::SameLine();
                    if (ImGui::Button(TX("Reset"), {third, button_h})) result = Reset;
                } else if (ImGui::Button(TX("Start game"), {2 * third + gap, button_h})) {
                    result = StartGame;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(TX("Quit"), {third, button_h})) result = Quit;
            }
#ifndef M2_MOBILE
            // the log: on by default, so a player can send it when something goes wrong
            ImGui::Spacing();
            ImGui::SeparatorText(TX("Logs"));
            if (ImGui::Checkbox(TX("Generate logs"), &cfg_.write_logs)) cfg_.save();
            hint(TX("What the game does, and each online race's measurements, kept in daytona.log (and the 5 sessions "
                    "before it). When something goes wrong, send it. Takes effect at the next start."));
            {
                const std::string folder = Config::pref_dir();
                ImGui::TextDisabled("%s", folder.c_str());
                if (ImGui::Button(TX("Open the logs folder"), {ImGui::CalcTextSize(TX("Open the logs folder")).x + 40.f * scale, button_h})) {
                    std::string url = "file://";
                    if (!folder.empty() && folder[0] != '/') url += "/"; // Windows: file:///C:/...
                    for (const char c : folder) url += c == '\\' ? '/' : c;
                    SDL_OpenURL(url.c_str());
                }
            }
            // a new version (app/update_check.h): the PC home's notice
            if (from_home) {
                ImGui::Spacing();
                ImGui::SeparatorText(TX("Updates"));
                if (ImGui::Checkbox(TX("Check for new versions"), &cfg_.update_check)) cfg_.save();
                hint(TX("Asks GitHub once a day whether a newer version is out, and says so on the home screen. "
                        "Nothing about you or this computer is sent."));
                if (!cfg_.update_latest.empty()) hint(TX("Latest version: %s"), cfg_.update_latest.c_str());
            }
#endif
            ImGui::EndTabItem();
        }

        // ---- Display: the picture and the window, the frame pacing
        if (ImGui::BeginTabItem(TX("Display"), nullptr, tab_flags(1))) {
            ImGui::Spacing();
            columns("##display", wide, [&] {
                ImGui::SeparatorText(TX("Picture"));
                {   // the picture's format first, easy to find (viewers' request, Daytona-Reborn issues 1 and 2)
                    const char *aspects[] = {TX("Arcade 4:3 (the original cabinet)"), TX("Automatic"), "16:10", "16:9",
                                             TX("21:9 (ultrawide)")};
                    static const char *aspect_ids[] = {"4:3", "", "16:10", "16:9", "21:9"};
                    int aspect = 1;
                    for (int i = 0; i < 5; i++)
                        if (cfg_.aspect == aspect_ids[i]) aspect = i;
                    if (combo(TX("Picture format"), "##aspect", &aspect, aspects, 5)) {
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
                if (combo(TX("Graphics API (restart the game)"), "##api", &api, apis, 4)) {
                    cfg_.gpu = api_ids[api];
                    cfg_.save();
                }
                const char *renderers[] = {TX("Software (exact)"), TX("Hardware (experimental)")};
                int rd = cfg_.renderer == "hardware" ? 1 : 0;
                if (combo(TX("Renderer"), "##renderer", &rd, renderers, 2)) {
                    cfg_.renderer = rd ? "hardware" : "software";
                    cfg_.save();
                }
                hint(TX("Software: the 3D is computed by the processor, as on the arcade board. Hardware: by the graphics card; in development, very close but not yet pixel exact."));
                const char *supersampling[] = {TX("Native 2x"), TX("Disabled"), "2x", "3x", "4x", TX("Native direct")};
                ImGui::BeginDisabled(cfg_.renderer != "hardware");
                int ss = std::clamp(cfg_.supersampling, 0, app::Config::kNativeDirect);
                if (combo(TX("Render resolution"), "##supersampling", &ss, supersampling, IM_ARRAYSIZE(supersampling))) {
                    cfg_.supersampling = ss;
                    cfg_.save();
                }
                ImGui::EndDisabled();
                hint(TX("The native modes follow the window under Direct3D 12 and Vulkan.\n"
                        "2x to 4x enlarge the original resolution."));
                const char *draw_modes[] = {TX("Double buffer"), TX("Single buffer"), TX("One frame in three")};
                int dm = std::clamp(cfg_.draw_mode, 0, 2);
                if (combo(TX("Draw mode"), "##draw_mode", &dm, draw_modes, 3)) {
                    cfg_.draw_mode = dm;
                    cfg_.save();
                }
                hint(TX("Double buffer: every frame is drawn, as in the game. Single buffer: one in two; one frame in three: one in three. Faster; the game itself is not slowed down."));
            }, [&] {
                ImGui::SeparatorText(TX("Window"));
                if (ImGui::Checkbox(TX("Fullscreen"), &cfg_.fullscreen)) {
                    SDL_SetWindowFullscreen(window_, cfg_.fullscreen);
                    cfg_.save();
                }
                if (ImGui::Checkbox(TX("Hide the mouse during play"), &cfg_.hide_cursor)) cfg_.save();
                ImGui::SetItemTooltip("%s", TX("No mouse pointer during the race. It comes back when this launcher\n"
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
                    ImGui::TextUnformatted(TX("Fullscreen mode"));
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##fullscreen_mode", preview.c_str())) {
                        if (ImGui::Selectable(TX("Desktop (borderless)"), cfg_.fullscreen_mode.empty())) choose("");
                        for (int i = 0; i < count; ++i) {
                            const std::string key = mode_key(*modes[i]);
                            ImGui::PushID(i);
                            if (ImGui::Selectable(mode_label(*modes[i]).c_str(), key == cfg_.fullscreen_mode)) choose(key);
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip("%s", TX("Desktop: fullscreen at the desktop's resolution and refresh rate.\n"
                                             "A mode: exclusive fullscreen at that resolution and rate, for example\n"
                                             "a 57.52 Hz mode created in the graphics driver's settings."));
                    SDL_free(modes);
                }
                if (ImGui::Checkbox(TX("Skip the launcher"), &cfg_.skip_launcher)) cfg_.save();
                hint(TX("(starts the game right away; Esc opens this launcher)"));

                ImGui::Spacing();
                ImGui::SeparatorText(TX("Display pacing"));
                hint(TX("(all unchecked: the cabinet's speed on any display)"));
                if (ImGui::Checkbox(TX("Smooth pacing on a 57.52 Hz display"), &cfg_.pace_smooth)) cfg_.save();
                ImGui::SetItemTooltip("%s", TX("On a display set to 57.52 Hz (or 115.05 Hz), one game frame per refresh (or two):\n"
                                         "no doubled or skipped frame. The speed stays the cabinet's, within 1 %.\n"
                                         "No effect at other rates."));
                if (ImGui::Checkbox(TX("Lock to the display (changes the game's speed)"), &cfg_.pace_sync_display)) cfg_.save();
                ImGui::SetItemTooltip("%s", TX("Runs the game at a rate that divides the display's exactly: each frame\n"
                                         "stays on screen the same time and motion is perfectly smooth. On a\n"
                                         "60, 120, 180 or 240 Hz display, the game runs at 60 frames/s, about 4 % faster than the cabinet\n"
                                         "(57.52); with the reference sound, the sound is a little faster too. No effect\n"
                                         "on a 144 or 165 Hz display, which cannot lock a rate close to the cabinet's:\n"
                                         "use VRR pacing then if the display supports G-Sync or FreeSync.\n"
                                         "Unchecked: the game always runs at the cabinet's speed."));
                if (ImGui::Checkbox(TX("VRR pacing (G-Sync / FreeSync)"), &cfg_.pace_vrr)) cfg_.save();
                ImGui::SetItemTooltip("%s", TX("For a variable refresh display: each frame lasts exactly 1/57.52 s, the display\n"
                                         "refreshes at the game's pace and each frame stays on screen the same time.\n"
                                         "VRR must be on in the display and the graphics driver; on a fixed\n"
                                         "rate display, the result looks like the default setting."));
                hint(TX("Currently: %s."), pacing_status_.c_str());
            });
            ImGui::EndTabItem();
        }

        // ---- Enhancements: off by default (rules.md); the scenery, widescreen, colours, smoothing
        if (ImGui::BeginTabItem(TX("Enhancements"), nullptr, tab_flags(2))) {
            ImGui::Spacing();
            columns("##enhancements", wide, [&] {
                ImGui::SeparatorText(TX("Scenery"));
                const char *distances[] = {TX("Shortest"), TX("Shorter"), TX("Default"), TX("Farther"), TX("Farthest")};
                int dd = std::clamp(cfg_.draw_distance, -2, 2);
                if (slider(TX("Draw distance"), "##draw_distance", &dd, -2, 2, distances[dd + 2])) {
                    cfg_.draw_distance = dd;
                    cfg_.save();
                }
                hint(TX("The scenery around the track. Widescreen also adds the scenery at the sides.\n"
                        "Shorter reduces the range; farther adds trees and buildings, not the road."));
                if (ImGui::Checkbox(TX("3D scenery"), &cfg_.scenery_3d)) cfg_.save();
                hint(TX("Fir trees in 3D, full grandstands, pillars under the Dinosaur Canyon bridge.\n"
                        "Applies the next time the game starts."));
                if (ImGui::Checkbox(TX("Opponent cars always detailed"), &cfg_.cars_near_model)) cfg_.save();
                hint(TX("One model, the most detailed, at every distance: no more cars changing look as they come closer. Applies the next time the game starts."));
                const int scenery_width = 496 + 2 * rt::GameLoop::wide_margin(cfg_.arcade_43() ? 0.0 : cfg_.aspect_ratio());
                const uint32_t automatic_budget = rt::automatic_scenery_budget(scenery_width, cfg_.draw_distance);
                int budget_mode = cfg_.draw_budget ? 1 : 0;
                const char *budget_modes[] = {TX("Automatic"), TX("Custom")};
                if (combo(TX("Polygon budget"), "##budget", &budget_mode, budget_modes, 2)) {
                    cfg_.draw_budget = budget_mode ? automatic_budget : 0;
                    cfg_.save();
                }
                if (cfg_.draw_budget) {
                    int value = int(cfg_.draw_budget);
                    ImGui::TextUnformatted(TX("Custom allowance"));
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt("##allowance", &value, 500, 5000)) {
                        cfg_.draw_budget = uint32_t(std::clamp(value, 1, int(rt::kMaxSceneryBudget)));
                        cfg_.save();
                    }
                    hint(TX("Replaces the automatic allowance. From 1 to 1,000,000.\n"
                            "Too low, scenery may be missing; too high, the game may slow down."));
                } else {
                    hint(TX("Current allowance: %u. It follows widescreen and the draw distance.\n"
                            "Resolution and supersampling do not change it."), automatic_budget);
                }
            }, [&] {
                ImGui::SeparatorText(TX("Widescreen"));
                ImGui::BeginDisabled(cfg_.arcade_43());
                if (ImGui::Checkbox(TX("Gauges at the screen edges (experimental)"), &cfg_.hud_edges)) cfg_.save();
                if (ImGui::Checkbox(TX("Stretch the sky backdrop (experimental)"), &cfg_.stretch_backdrop)) cfg_.save();
                ImGui::EndDisabled();
                hint(TX("Shows more of the scene at the sides. The gauges stay in 4:3 in the centre, or else lap times, position and maps move to the edges. In the race, the sky at the sides is plain blue, or else the game's sky picture stretched across the screen."));

                ImGui::Spacing();
                ImGui::SeparatorText(TX("Image quality"));
                if (ImGui::Button(TX("Maximum quality (graphics card)"), ImVec2(-FLT_MIN, 0))) {
                    cfg_.renderer = "hardware";
#ifdef __APPLE__
                    cfg_.supersampling = 4; // the native modes: Direct3D 12 and Vulkan
#else
                    cfg_.supersampling = app::Config::kNativeResolution;
#endif
                    cfg_.msaa = 4;
                    cfg_.aniso = 16;
                    cfg_.smooth_cars = cfg_.car_reflections = cfg_.sharp_car_textures = cfg_.car_shadows = true;
                    cfg_.save();
                }
                hint(TX("The Hardware renderer at the screen's resolution and every option below, for a recent graphics card. "
                        "Some apply the next time the game starts."));
                if (ImGui::Button(TX("Detect automatically"), ImVec2(-FLT_MIN, 0))) quality_request_ = true;
                hint(TX("Measures what the graphics card can draw, in a few seconds, and recommends a level."));
                ImGui::BeginDisabled(cfg_.renderer != "hardware");
                const char *msaa_modes[] = {TX("Off"), "2x", "4x", "8x"};
                int msaa = cfg_.msaa >= 8 ? 3 : cfg_.msaa >= 4 ? 2 : cfg_.msaa >= 2 ? 1 : 0;
                if (combo(TX("Antialiasing (MSAA)"), "##msaa", &msaa, msaa_modes, 4)) {
                    cfg_.msaa = 1 << msaa;
                    cfg_.save();
                }
                hint(TX("Smooths the edges of the polygons and of the cut-out textures (trees, tyre marks).\n"
                        "Not with the ray-traced sun shadows. Applies the next time the game starts."));
                const char *filters[] = {TX("The game's own"), TX("Anisotropic 2x"), TX("Anisotropic 4x"),
                                         TX("Anisotropic 8x"), TX("Anisotropic 16x")};
                int filter = cfg_.aniso >= 16 ? 4 : cfg_.aniso >= 8 ? 3 : cfg_.aniso >= 4 ? 2 : cfg_.aniso >= 2 ? 1 : 0;
                if (combo(TX("Texture filtering"), "##aniso", &filter, filters, 5)) {
                    cfg_.aniso = filter ? 1 << filter : 1;
                    cfg_.save();
                }
                hint(TX("Anisotropic: the road and the walls stay sharp into the distance, without shimmering."));
                if (ImGui::Checkbox(TX("Smooth car shading"), &cfg_.smooth_cars)) cfg_.save();
                hint(TX("The light on the cars blends across their faces instead of one shade a face: rounder bodies."));
                if (ImGui::Checkbox(TX("Paint reflections"), &cfg_.car_reflections)) cfg_.save();
                hint(TX("The cars' bodies reflect the sky and the sun, more at grazing angles."));
                if (ImGui::Checkbox(TX("Sharp car textures up close"), &cfg_.sharp_car_textures)) cfg_.save();
                hint(TX("The cars' numbers and logos keep crisp, rounded edges when seen up close, instead of a blur."));
                if (ImGui::Checkbox(TX("Smooth menus and HUD"), &cfg_.smooth_2d)) cfg_.save();
                hint(TX("The game's 2D pictures (menus, speedometer, lap times, results) get smooth curves and diagonals "
                        "at high resolution instead of square pixels. Their colours and dithering stay the game's."));
#ifndef M2_MOBILE
                { // the player's car (experimental): a body from <data folder>/cars/NAME/
                    static std::vector<std::string> cars;
                    static double listed = -1;
                    if (ImGui::GetTime() - listed > 2.0 || listed < 0) { // the folder, again now and then
                        listed = ImGui::GetTime();
                        cars.clear();
                        std::error_code ec;
                        for (const auto &e : std::filesystem::directory_iterator(Config::pref_dir() + "cars", ec))
                            if (e.is_directory(ec) && std::filesystem::exists(e.path() / "bodies.m2car", ec))
                                cars.push_back(e.path().filename().string());
                        std::sort(cars.begin(), cars.end());
                    }
                    ImGui::TextUnformatted(TX("Player's car (experimental)"));
                    ImGui::SetNextItemWidth(std::min(360.f * scale, ImGui::GetContentRegionAvail().x));
                    const std::string current = cfg_.player_car.empty() ? std::string(TX("The game's own")) : cfg_.player_car;
                    if (ImGui::BeginCombo("##player_car", current.c_str())) {
                        if (ImGui::Selectable(TX("The game's own"), cfg_.player_car.empty())) cfg_.player_car.clear(), cfg_.save();
                        for (const auto &c : cars)
                            if (ImGui::Selectable(c.c_str(), c == cfg_.player_car)) cfg_.player_car = c, cfg_.save();
                        ImGui::EndCombo();
                    }
                    char where[1200];
                    std::snprintf(where, sizeof where,
                                  TX("A car body you made, in a folder of %scars (docs/CUSTOM-CARS.md). The game's wheels, "
                                     "light and handling are kept. Takes effect at the next start."),
                                  Config::pref_dir().c_str());
                    hint("%s", where);
                }
#endif
                if (ImGui::Checkbox(TX("Car shadows"), &cfg_.car_shadows)) cfg_.save();
                hint(TX("The cars near the camera cast shadows from the game's sun on the road and the walls. Any graphics card."));
                ImGui::Indent();
                ImGui::BeginDisabled(!cfg_.car_shadows);
                if (ImGui::Checkbox(TX("In place of the game's own shadow"), &cfg_.replace_game_shadow)) cfg_.save();
                ImGui::EndDisabled();
                ImGui::Unindent();
                hint(TX("The game's simple shadow under the car (a checkered patch) is left out while the cars' shadows are drawn."));
                ImGui::EndDisabled();
                hint(TX("These need the Hardware renderer (Display tab)."));

                ImGui::Spacing();
                ImGui::SeparatorText(TX("Screen effects"));
                if (ImGui::Checkbox(TX("Bloom"), &cfg_.bloom)) cfg_.save();
                if (cfg_.bloom && slider(TX("Bloom strength"), "##bloom_strength", &cfg_.bloom_strength, 10, 100, "%d%%"))
                    cfg_.save();
                hint(TX("The bright parts of the picture (the sky, the lights, white lines) glow softly."));
                if (ImGui::Checkbox(TX("Arcade monitor (CRT)"), &cfg_.crt_filter)) cfg_.save();
                if (cfg_.crt_filter && slider(TX("Monitor strength"), "##crt_strength", &cfg_.crt_strength, 20, 100, "%d%%"))
                    cfg_.save();
                hint(TX("The look of the cabinet's tube: the game's scanlines, the colour grille and darker corners. "
                        "Best on a large or high-resolution screen."));

                ImGui::Spacing();
                ImGui::SeparatorText(TX("Colours"));
                int crt = cfg_.display_gamma >= 2.0f ? int(cfg_.display_gamma * 10.0f + 0.5f) : 20;
                char crt_label[32];
                std::snprintf(crt_label, sizeof crt_label, crt <= 20 ? TX("PC (original)") : TX("Cabinet %d.%d"), crt / 10, crt % 10);
                if (slider(TX("Screen colours (restart the game)"), "##gamma", &crt, 20, 26, crt_label)) {
                    cfg_.display_gamma = crt <= 20 ? 0.0f : float(crt) / 10.0f;
                    cfg_.save();
                }
                hint(TX("PC: the game's colours as before (original picture).\n"
                        "Cabinet 2.2 to 2.6: the same colours as on an arcade cabinet's screen, deeper and less washed out. Recommended: 2.4."));
                int saturation = int(cfg_.display_saturation * 100.0f + 0.5f);
                if (slider(TX("Colour saturation (restart the game)"), "##saturation", &saturation, 50, 150,
                           saturation == 100 ? TX("100 %% (original)") : "%d %%")) {
                    cfg_.display_saturation = float(saturation) / 100.0f;
                    cfg_.save();
                }
                hint(TX("Above 100 %%: livelier colours. 100 %%: the game's colours as they are."));

                ImGui::Spacing();
                ImGui::SeparatorText(TX("Smoothing"));
                ImGui::BeginDisabled(cfg_.renderer != "hardware" || cfg_.draw_mode != 0);
#ifdef M2_RAY_TRACING
                if (ImGui::Checkbox(TX("Sun shadows (ray tracing, experimental)"), &cfg_.sun_shadows)) cfg_.save();
                hint(TX("Shadows cast by the game's sun (Direct3D 12, ray tracing capable card, native resolution). Applies the next time the game starts."));
                if (cfg_.sun_shadows) {
                    if (ImGui::Checkbox(TX("Sky light (experimental)"), &cfg_.sky_light)) cfg_.save();
                    hint(TX("The sky lights from everywhere: darker under the cars, under the gantry, at the foot of walls. Applies the next time the game starts."));
                    int tens = cfg_.sun_height / 10;
                    char sun_label[32];
                    std::snprintf(sun_label, sizeof sun_label, tens ? TX("%d degrees") : TX("the game's own"), tens * 10);
                    if (slider(TX("Sun height"), "##sun_height", &tens, 0, 8, sun_label)) {
                        cfg_.sun_height = tens * 10;
                        cfg_.save();
                    }
                    hint(TX("The direction stays the game's sun's. Lower, the shadows get longer (as in late afternoon). Applies the next time the game starts."));
                }
#endif
                if (ImGui::Checkbox(TX("120 frames/s smoothing (experimental)"), &cfg_.interpolate)) cfg_.save();
                ImGui::EndDisabled();
                hint(TX("In-between frames between the game's. Needs the Hardware renderer and the double buffer.\n"
                        "Applies the next time the game starts."));
            });
            ImGui::EndTabItem();
        }

        // ---- Link play: cabinets linked directly, without the relay (advanced: the
        // Online mode in the home menu covers racing over the Internet)
        if (ImGui::BeginTabItem(TX("Link play (advanced)"), nullptr, tab_flags(3))) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.74f, 0.45f, 1));
            ImGui::TextWrapped("%s", TX("To race friends over the Internet, use Online in the home menu: no port to open, "
                                        "player names, standings, the room's settings for everyone. Link play joins "
                                        "cabinets directly, on a local network, or through the daytona-link helper."));
            ImGui::PopStyleColor();
            ImGui::SeparatorText(TX("Link play (experimental, reset)"));
            if (ImGui::Checkbox(TX("Link to other cabinets"), &cfg_.link)) cfg_.save();
            ImGui::BeginDisabled(!cfg_.link);
            columns("##link", wide, [&] {
                ImGui::TextUnformatted(TX("This cabinet's port"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##link_port", &cfg_.link_port, 0, 0)) {
                    cfg_.link_port = std::clamp(cfg_.link_port, 1, 65535);
                    cfg_.save();
                }
            }, [&] {
                ImGui::TextUnformatted(TX("Next cabinet (host:port)"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##link_next", link_next_buf_, sizeof link_next_buf_)) {
                    cfg_.link_next = link_next_buf_;
                    cfg_.save();
                }
            });
            if (ImGui::Checkbox(TX("Synchronise frames (each cabinet waits for the master)"), &cfg_.link_framesync)) cfg_.save();
            hint("%s", TX("On a local network only: every frame waits for the data to go round all the cabinets, so "
                          "over the Internet the game slows down (22 frames/s with 20 ms between two cabinets)."));
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::TextWrapped(TX("Network: %s"), link_status_.c_str());
            ImGui::Spacing();
            hint(TX("The cabinets link in a ring over the network (Wi-Fi or cable): each listens on its port and connects to the next one; with two, each has the other as next. All the computers can take the same port, for example 15112, and Next cabinet = the other computer's address:15112. In test mode (F2) > GAME SYSTEM, set LINK ID (one MASTER, the others SLAVE) and a different CAR NUMBER on each. The 1994 game (revision A) supports link play."));
            ImGui::EndTabItem();
        }

        // ---- Controls: devices, steering, force feedback, the assignments
        if (ImGui::BeginTabItem(TX("Controls"), nullptr, tab_flags(4))) {
            Controls &c = cfg_.controls;
            ImGui::Spacing();
            columns("##controls", wide, [&] {
                ImGui::SeparatorText(TX("Devices"));
                ImGui::TextWrapped(TX("Gamepad: %s"), pad ? SDL_GetGamepadName(pad) : TX("none connected"));
                std::string joys;
                for (SDL_Joystick *j : devices.joys) {
                    const char *n = SDL_GetJoystickName(j);
                    joys += (joys.empty() ? "" : ", ") + std::string(n ? n : "joystick");
                }
                ImGui::TextWrapped(TX("Wheels and joysticks: %s"), joys.empty() ? TX("none found") : joys.c_str());
                ImGui::Spacing();
                // live meters
                const bool *keys = SDL_GetKeyboardState(nullptr);
                const float steer = c.value(SteerRight, keys, devices) - c.value(SteerLeft, keys, devices);
                char label[32];
                std::snprintf(label, sizeof label, "%+.2f", c.steer_invert ? -steer : steer);
                ImGui::TextUnformatted(TX("Steering"));
                ImGui::ProgressBar(0.5f + 0.5f * (c.steer_invert ? -steer : steer), {-FLT_MIN, 0}, label);
                ImGui::TextUnformatted(TX("Accelerator"));
                ImGui::ProgressBar(c.value(Accelerate, keys, devices), {-FLT_MIN, 0});
                ImGui::TextUnformatted(TX("Brake pedal"));
                ImGui::ProgressBar(c.value(Brake, keys, devices), {-FLT_MIN, 0});
            }, [&] {
                ImGui::SeparatorText(TX("Steering"));
                ImGui::TextUnformatted(TX("Dead zone"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##deadzone", &c.deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
                if (ImGui::Checkbox(TX("Invert steering"), &c.steer_invert)) cfg_.save();
                ImGui::TextUnformatted(TX("Wheel dead zone (experimental)"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##joy_deadzone", &c.joy_deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
                int wheel_sensitivity = int(c.wheel_sensitivity * 100.0f + 0.5f);
                if (slider(TX("Wheel sensitivity"), "##wheel_sensitivity", &wheel_sensitivity, 25, 300, "%d%%")) {
                    c.wheel_sensitivity = float(wheel_sensitivity) / 100.0f;
                    cfg_.save();
                }
                hint(TX("100 %%: linear. Lower: gentler near the centre. Higher: sharper.\n"
                        "Only for the steering axis of a wheel or joystick; full lock is kept."));
                int pad_sensitivity = int(c.pad_steer_sensitivity * 100.0f + 0.5f);
                if (slider(TX("Stick sensitivity (gamepad)"), "##pad_sensitivity", &pad_sensitivity, 25, 100, "%d%%")) {
                    c.pad_steer_sensitivity = float(pad_sensitivity) / 100.0f;
                    cfg_.save();
                }
                int pad_speed = int(c.pad_steer_speed * 100.0f + 0.5f);
                if (slider(TX("Steering speed (gamepad)"), "##pad_speed", &pad_speed, 2, 100,
                           pad_speed >= 100 ? TX("Immediate") : "%d%%")) {
                    c.pad_steer_speed = float(pad_speed) / 100.0f;
                    cfg_.save();
                }
                hint(TX("Gamepad stick only. Sensitivity: lower, gentler near the centre (full lock kept).\n"
                        "Speed: how fast the game's wheel follows the stick, per frame (100 %%: at once)."));
            });

            ImGui::Spacing();
            columns("##feedback", wide, [&] {
                ImGui::SeparatorText(TX("Force feedback"));
                int ffb = int(cfg_.ffb_strength * 100.0f + 0.5f);
                if (slider(TX("Force feedback (experimental)"), "##ffb", &ffb, 0, 100, ffb ? "%d%%" : TX("Off"))) {
                    cfg_.ffb_strength = float(ffb) / 100.0f;
                    cfg_.save();
                }
                if (ImGui::Checkbox(TX("Invert the force"), &cfg_.ffb_invert)) cfg_.save();
                ImGui::SameLine();
                if (ImGui::Checkbox(TX("Force feedback log"), &cfg_.ffb_log)) cfg_.save();
#ifdef __linux__ // for Linux drivers without a spring effect (the kernel's Logitech driver)
                if (ImGui::Checkbox(TX("Centring computed by the game"), &cfg_.ffb_centring_override)) cfg_.save();
                ImGui::SetItemTooltip("%s", TX("For a wheel that kicks on impacts but does not recentre in bends: its\n"
                                         "Linux driver has no spring effect (the log's first line says \"spring no\").\n"
                                         "The game's recentring is then computed here from the wheel's position and\n"
                                         "sent as a constant force. Leave unchecked if the wheel already recentres."));
#endif
            }, [&] {
                hint(TX("The cabinet wheel's motor (recentring, resistance, wheel pulling), on the device assigned to steering; a gamepad rumbles when the car is pushed. Currently: %s.\n"
                        "Wheels and force feedback are not yet tested on real hardware: feedback welcome.\n"
                        "The log goes to daytona.log, next to launcher.ini under Windows, else to the terminal."), ffb_device_);
#ifndef _WIN32 // SDL's own Logitech driver is off on Windows already
                if (ImGui::Checkbox(TX("Legacy Logitech wheel support (restart the game)"), &cfg_.legacy_logitech_wheels)) cfg_.save();
                hint(TX("Logitech wheels through the system driver: needed for a wheel that is listed but does nothing when assigned (the original Driving Force). Unchecked: SDL's driver, which gives force feedback to recent wheels under macOS. After a change, restart and assign the wheel's controls again."));
#endif
            });

            ImGui::Spacing();
            ImGui::SeparatorText(TX("Assignments"));
            hint(TX("Triggers, sticks, wheels and pedals are analogue. To assign a wheel or pedal axis, click its button, then turn the wheel or press the pedal as far as you want full lock or full travel, and let go: that sets its range."));
            if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                              ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(TX("Control"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(TX("Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(TX("Gamepad"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn(TX("Wheel / joystick (experimental)"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
                ImGui::TableHeadersRow();
                // a button filling its cell, but for a small "clear" beside it
                auto cell_button = [&](const char *text, const char *clear_id, bool &clear) {
                    const float clear_w = ImGui::CalcTextSize(TX("clear")).x + ImGui::GetStyle().FramePadding.x * 2;
                    const bool pressed = ImGui::Button(text, {std::max(40.f, ImGui::GetContentRegionAvail().x - clear_w -
                                                                                  ImGui::GetStyle().ItemSpacing.x), 0});
                    ImGui::SameLine();
                    clear = ImGui::SmallButton(clear_id);
                    return pressed;
                };
                for (int a = 0; a < kNumActions; a++) {
                    ImGui::PushID(a);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(action_name(Action(a)));
                    ImGui::TableNextColumn();
                    const bool cap_key = capture_action_ == a && capture_kind_ == CaptureKey;
                    const char *kn = c.bind[a].key == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(c.bind[a].key);
                    if (ImGui::Button(cap_key ? TX("Press a key...") : kn, {-FLT_MIN, 0})) start_capture(a, CaptureKey, devices);
                    ImGui::TableNextColumn();
                    const bool cap_pad = capture_action_ == a && capture_kind_ == CapturePad;
                    const std::string pn = cap_pad ? TX("Press or move...") : c.bind[a].pad.describe();
                    bool clear = false;
                    if (cell_button(pn.c_str(), TX("clear"), clear)) start_capture(a, CapturePad, devices);
                    if (clear) {
                        c.bind[a].pad = PadInput{};
                        cfg_.save();
                    }
                    ImGui::TableNextColumn();
                    const bool cap_joy = capture_action_ == a && capture_kind_ == CaptureJoy;
                    const std::string jn = !cap_joy ? c.bind[a].joy.describe(devices)
                                           : joy_tracking_ ? TX("As far as wanted, then let go...")
                                                           : TX("Press, or move all the way and let go...");
                    if (cell_button(jn.c_str(), TX("clear##joy"), clear)) start_capture(a, CaptureJoy, devices);
                    if (clear) {
                        c.bind[a].joy = JoyInput{};
                        cfg_.save();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            hint(TX("Esc cancels an assignment; Backspace clears a key."));
            if (ImGui::Button(TX("Reset the default settings"), {0, button_h})) {
                c.set_defaults();
                cfg_.save();
            }
            ImGui::EndTabItem();
        }

        // ---- Sound
#ifndef M2_MOBILE
        if (ImGui::BeginTabItem(TX("AI textures"), nullptr, tab_flags(6))) {
            draw_hires_tab(scale);
            ImGui::EndTabItem();
        }
#endif
        if (ImGui::BeginTabItem(TX("Sound"), nullptr, tab_flags(5))) {
            ImGui::Spacing();
            auto percent = [&](const char *label, const char *id, float &value) { // a 0..1 setting as 0-100%
                int v = int(value * 100.0f + 0.5f);
                if (slider(label, id, &v, 0, 100, "%d%%")) {
                    value = float(v) / 100.0f;
                    cfg_.save();
                }
            };
            columns("##sound", wide, [&] {
                ImGui::SeparatorText(TX("Volume"));
                percent(TX("Volume"), "##volume", cfg_.volume);
                if (ImGui::Checkbox(TX("Mute"), &cfg_.mute)) cfg_.save();
                ImGui::Spacing();
                percent(TX("Music"), "##music", cfg_.music_volume);
                percent(TX("Effects"), "##effects", cfg_.effects_volume);
                hint(TX("The balance between the music and everything else (engine, skids, impacts).\n"
                        "100 %% and 100 %%: the cabinet sound board's mix."));
            }, [&] {
                ImGui::SeparatorText(TX("Engine"));
                if (ImGui::Checkbox(TX("Native sound (experimental, reset)"), &cfg_.native_audio)) cfg_.save();
                hint(TX("Native sequencer and mixer; the reference sound stays available for comparison."));
            });
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        open_tab = -1;
    }
    ImGui::PopTextWrapPos();
#ifdef M2_MOBILE
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(0))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
#endif
    ImGui::EndChild();
#ifndef M2_MOBILE
    if (neon_look) { // Back, as on the room's settings: at the panel's foot, on the right
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
        const float back_w = 180 * scale;
        if (neon::main_button(ImGui::GetWindowDrawList(), body_font_, scale, TX("Back"), {right - back_w, at.y + 12 * scale},
                              {right, at.y + 76 * scale}, false, 28))
            result = Back;
    }
#endif
    ImGui::End();
#ifndef M2_MOBILE
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
    ImGui::PopStyleVar(neon_vars);
    ImGui::PopStyleColor(neon_colors);
#endif
    return result;
}

} // namespace app
