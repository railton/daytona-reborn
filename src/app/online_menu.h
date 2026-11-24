// The home menu's Online screen: join a room with an invite code, or connect
// to a daytona-relay server, choose the room's race settings and create it
// (its invite code to pass on); then the room's players, until its owner
// starts the race.
#pragma once

#include "app/config.h"
#include "app/online.h"

#include <memory>
#include <string>
#include <vector>

namespace app {

class RelayLink;

class OnlineMenu {
public:
    enum Action { Stay, Back, Connect, Leave, Resume, Begin };
    OnlineMenu();
    ~OnlineMenu();
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
    // Create: the server asked for its defaults (Connect), then the room's settings
    std::unique_ptr<RelayLink> probe_; // the info request in flight
    std::string probed_;               // the server (and password) that answered; empty: not connected
    std::vector<int> choice_;          // per room option (room_options() in the .cpp), the value picked
    bool ask_password_ = false;        // the server asked for one (or one is saved): show its field
    void load(const Config &cfg);
    void connected(const Config &cfg, const std::vector<std::pair<std::string, std::string>> &defaults);
    std::string room_settings() const;
};

} // namespace app
