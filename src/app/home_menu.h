#pragma once

#include "runtime/race_request.h"

#include <string>
#include <vector>

struct ImFont;

namespace app {

// What the home screen shows of the Online mode: the saved player and
// server, the server's last answer, and the room this game is in.
struct HomeOnline {
    std::string name, server; // saved (Config::online_name, online_server)
    std::string settings;     // the last room's settings, "key:value,..." (Config::online_room)
    enum class Probe { None, Testing, Ok, Offline, Version } probe = Probe::None;
    int latency_ms = -1; // round trip to the server: the room's, else the probe's
    // the room: none, connecting to it, or in it
    enum class Room { None, Connecting, In } room = Room::None;
    std::string code; // its invite code
    int joined = 0;   // players in it
    bool owner = false, racing = false, me_in_race = false, game_running = false;
    int me = 0;                     // this player's line in names (1..), 0: waiting
    std::vector<std::string> names; // the players, the room's order (the owner first)
    std::string error;              // the room's or the server's last refusal
};

class HomeMenu {
public:
    enum Action {
        Stay, Play, Resume, Settings, Quit, Cancel,
        Online,     // the full Online screen: the server, the room's settings, the room's details
        JoinRoom,   // invite() holds a valid code
        CreateRoom, // on the saved server, with the last room's settings
        StartRace,  // the room's owner starts it
        LeaveRoom,
        SetName, // name() holds the name typed
    };
    void init();
    void select(rt::StartRaceRequest request) {
        course_ = request.course;
        transmission_ = request.transmission;
        mode_ = request.mode;
    }
    Action draw(bool has_race, bool loading, const char *message, float opacity, const HomeOnline &online = {});
    rt::RaceCourse course() const { return course_; }
    rt::StartRaceRequest request() const { return {course_, transmission_, mode_}; }
    const std::string &invite() const { return invite_text_; }
    const std::string &name() const { return name_text_; }
    // The Online card first (back from the Online screen; a capture).
    void show_online() { tab_ = Tab::Online; }
    ImFont *display_font() const { return display_; }
    ImFont *body_font() const { return body_; }
    // Diagnostics (a capture): an invite as if pasted.
    void paste_invite(const std::string &code);

private:
    enum class Screen { Ready, Loading, Paused };
    enum class Tab { Solo, Online };
    Screen screen_ = Screen::Ready;
    Tab tab_ = Tab::Solo;
    rt::RaceCourse course_ = rt::RaceCourse::Beginner;
    rt::RaceTransmission transmission_ = rt::RaceTransmission::Automatic;
    rt::RaceMode mode_ = rt::RaceMode::Normal;
    ImFont *display_ = nullptr, *body_ = nullptr;
    char invite_[256] = {}, name_[64] = {}, ask_[64] = {};
    std::string invite_text_, name_text_, seen_clip_;
    bool pasted_ = false;       // the invite came from the clipboard
    double clipboard_at_ = -10; // when the clipboard was last looked at
    bool invite_active_ = false; // its field being edited (last frame)
    bool ask_name_ = false, ask_leave_ = false, copied_ = false;
    Action pending_ = Stay; // Join or Create, once the name asked for is given
};

} // namespace app
