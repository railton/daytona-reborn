#include "app/home_menu.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

struct Screen {
    app::HomeMenu menu;
    SDL_Window *window;
    SDL_Gamepad *pad;
    bool has_race = false;
    bool loading = false;
    bool shown = true;

    Screen(SDL_Window *surface, SDL_Gamepad *gamepad) : window(surface), pad(gamepad) {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        require(ImGui_ImplSDL3_InitForOther(window), "ImGui SDL setup failed");
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, pad ? &pad : nullptr, pad ? 1 : 0);
        menu.init();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "font atlas is missing");
    }
    ~Screen() { ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext(); }

    app::HomeMenu::Action frame() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) ImGui_ImplSDL3_ProcessEvent(&event);
        ImGui_ImplSDL3_NewFrame();
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        const auto result = shown ? menu.draw(has_race, loading, nullptr, 1.f) : app::HomeMenu::Stay;
        ImGui::Render();
        return result;
    }
    app::HomeMenu::Action press(SDL_Scancode key, SDL_GamepadButton button) {
        app::HomeMenu::Action action = app::HomeMenu::Stay;
        for (bool down : {true, false}) {
            if (pad) {
                require(SDL_SetJoystickVirtualButton(SDL_GetGamepadJoystick(pad), button, down), "virtual button failed");
                SDL_UpdateJoysticks();
            } else {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.windowID = SDL_GetWindowID(window);
                event.key.scancode = key;
                event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
                event.key.down = down;
                require(ImGui_ImplSDL3_ProcessEvent(&event), "keyboard event was not handled");
            }
            const auto result = frame();
            if (result != app::HomeMenu::Stay) {
                require(action == app::HomeMenu::Stay, "a single press produced two actions");
                action = result;
            }
        }
        require(frame() == app::HomeMenu::Stay, "released input repeated an action");
        return action;
    }
    const char *focus() const {
        auto *home = ImGui::FindWindowByName("Daytona accueil");
        if (home) for (auto label : {"Jouer", "Reprendre", "Retour", "Automatique", "Manuelle", "01", "02", "03",
                                     "Réglages", "Quitter", "Nouvelle course"})
            if (home->GetID(label) == ImGui::GetCurrentContext()->NavId) return label;
        return "(none)";
    }
};

int main() try {
    require(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE), "cannot select SDL dummy video");
    require(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"), "cannot allow background test events");
    require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    auto *window = SDL_CreateWindow("Home input test", 1280, 720, SDL_WINDOW_HIDDEN);
    require(window, SDL_GetError());
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = "Home input virtual pad";
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    const auto id = SDL_AttachVirtualJoystick(&desc);
    require(id, SDL_GetError());
    auto *pad = SDL_OpenGamepad(id);
    require(pad, SDL_GetError());
    for (SDL_Gamepad *input : {static_cast<SDL_Gamepad *>(nullptr), pad}) {
        Screen screen(window, input);
        for (int i = 0; i < 3; ++i) screen.frame();
        auto expect_move = [&](SDL_Scancode key, SDL_GamepadButton button) {
            require(screen.press(key, button) == app::HomeMenu::Stay, "navigation launched an action");
        };
        auto confirm = [&]() { return screen.press(SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_SOUTH); };
        require(std::string_view(screen.focus()) == "Jouer", "home default action is not Play");
        expect_move(SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP);
        if (std::string_view(screen.focus()) == "Automatique") expect_move(SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        require(std::string_view(screen.focus()) == "Manuelle", "cannot focus Manual");
        require(confirm() == app::HomeMenu::Stay && screen.menu.request().transmission == rt::RaceTransmission::Manual,
                "Manual choice not applied");
        expect_move(SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        expect_move(SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        for (int i = 0; i < 2 && std::string_view(screen.focus()) != "03"; ++i)
            expect_move(SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        require(std::string_view(screen.focus()) == "03", "cannot focus Expert");
        require(confirm() == app::HomeMenu::Stay && screen.menu.request().course == rt::RaceCourse::Expert,
                "Expert choice not applied");
        expect_move(SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP);
        require(confirm() == app::HomeMenu::Play, "Expert Manual did not request Play");
        screen.loading = true;
        for (int i = 0; i < 3; ++i) screen.frame();
        require(confirm() == app::HomeMenu::Cancel, "default loading action did not cancel");
        screen.loading = false;
        for (int i = 0; i < 3; ++i) screen.frame();
        require(screen.menu.request().course == rt::RaceCourse::Expert &&
                screen.menu.request().transmission == rt::RaceTransmission::Manual,
                "cancel lost the selected race");
        require(confirm() == app::HomeMenu::Play, "play did not recover after cancelling load");
        screen.shown = false;
        for (int i = 0; i < 8; ++i) screen.frame();
        screen.has_race = true;
        screen.shown = true;
        for (int i = 0; i < 3; ++i) screen.frame();
        require(confirm() == app::HomeMenu::Resume, "default paused action did not resume");
        std::printf("%s: Expert Manual, cancel, play and resume passed\n", input ? "gamepad" : "keyboard");

    }
    SDL_CloseGamepad(pad);
    SDL_DetachVirtualJoystick(id);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr,"%s\n",error.what());
    return 1;
}
