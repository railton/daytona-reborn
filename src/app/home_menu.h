#pragma once

#include "runtime/race_request.h"

#include <algorithm>
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
    // who has to update to play online here: this game (the server races
    // with a newer one, or refused it as older), or the server (older)
    enum class Update { None, Game, Server } update = Update::None;
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
        OpenRelease,   // the new version's notice: its release page
        DismissUpdate, // the new version's notice: not again for that version
        LowerQuality,  // the slow race's notice (app/speed_check.h): lower what held it back
        DismissSlow,   // the slow race's notice: closed
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
    // Solo: the cabinet's country (0 USA, 1 JPN, 2 EXPORT) and difficulty
    // (0 easy .. 3 hardest), chosen on the Solo card (Config::solo_country...).
    int solo_country() const { return solo_country_; }
    int solo_difficulty() const { return solo_difficulty_; }
    void set_solo(int country, int difficulty) { solo_country_ = country, solo_difficulty_ = difficulty; }
    ImFont *display_font() const { return display_; }
    ImFont *body_font() const { return body_; }
    // "A new version is out" (app/update_check.h): its version ("0.19.0"),
    // shown in a card above the bottom bar; "" for none.
    void set_update(const std::string &version) { update_ = version; }
    // The first race was slow: what held it back, said on a card with "Lower the quality"; "" for none.
    void set_slow(const std::string &text) { slow_ = text; }
    // Diagnostics (a capture): an invite as if pasted.
    void paste_invite(const std::string &code);
    // A room this player made: its code is never pasted into Join on its own.
    void own_code(const std::string &code) {
        if (!code.empty() && std::find(own_codes_.begin(), own_codes_.end(), code) == own_codes_.end()) own_codes_.push_back(code);
    }

private:
    enum class Screen { Ready, Loading, Paused };
    enum class Tab { Solo, Online };
    Screen screen_ = Screen::Ready;
    Tab tab_ = Tab::Solo;
    rt::RaceCourse course_ = rt::RaceCourse::Beginner;
    rt::RaceTransmission transmission_ = rt::RaceTransmission::Automatic;
    rt::RaceMode mode_ = rt::RaceMode::Normal;
    int solo_country_ = 0, solo_difficulty_ = 1;
    ImFont *display_ = nullptr, *body_ = nullptr;
    char invite_[256] = {}, name_[64] = {}, ask_[64] = {};
    std::string invite_text_, name_text_, seen_clip_;
    // the invite codes of the rooms this player made: copied to send to
    // friends (from the Online screen's Copy too), never pasted back into
    // the Join field on their own
    std::vector<std::string> own_codes_;
    std::string update_; // the new version's notice, "" for none
    std::string slow_;   // the slow race's notice, "" for none
    bool online_update_dismissed_ = false; // the Online mode's update notice, closed until the game restarts
    bool pasted_ = false;       // the invite came from the clipboard
    double clipboard_at_ = -10; // when the clipboard was last looked at
    bool invite_active_ = false; // its field being edited (last frame)
    bool ask_name_ = false, ask_leave_ = false, copied_ = false;
    Action pending_ = Stay; // Join or Create, once the name asked for is given
};

} // namespace app
