// The home menu's Online screen: create a room on a daytona-relay server
// (its invite code to pass on) or join one with a code; then the room's
// players until it is full, when the race starts on its own.
#pragma once

#include "app/config.h"
#include "app/online.h"

#include <string>

namespace app {

class RelayLink;

class OnlineMenu {
public:
    enum Action { Stay, Back, Connect, Leave, Resume, Begin };
    // link: the current room's connection, or null; racing: its game runs.
    Action draw(Config &cfg, const RelayLink *link, bool racing, const std::string &message);
    // After Connect: where, and how to introduce ourselves.
    const std::string &server() const { return server_; }
    const online::Hello &hello() const { return hello_; }

private:
    bool init_ = false;
    char name_[64] = {}, server_buf_[128] = {}, password_[64] = {}, invite_[256] = {};
    std::string server_, error_;
    online::Hello hello_;
    std::string room_password_; // the password of the room we are in (for its invite)
    bool copied_ = false;
    void load(const Config &cfg);
};

} // namespace app
