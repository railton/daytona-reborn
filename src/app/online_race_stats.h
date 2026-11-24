// An online race's own measurements, for the log (daytona.log), so a
// player's log says what a race was like when something went wrong, as
// m2run --relay says it in tests:
//
//   - how the previous car's data moved each frame (byte 0x0a of the block
//     received at 0x21c0, that cabinet's tick counter): 1 is smooth, 0 a
//     frame with no new data (the car stalls), 2+ a skip (it jumps);
//   - the game's speed: race frames run against the arcade's 57.524 a second;
//   - the link's reserve at the end and how often it changed;
//   - the relay link's sends and messages a second, and the messages UDP
//     brought first;
//   - the time spent in the menu (Esc) during the race.
#pragma once

#include "app/relay_link.h"
#include "runtime/comm_board.h"
#include "runtime/m2_board.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace app {

class OnlineRaceStats {
public:
    // The race began (this frame): counters from here.
    void begin(const RelayLink &link, uint64_t now_ns) {
        *this = OnlineRaceStats{};
        active_ = true;
        from_ns_ = last_ns_ = now_ns;
        sends_from_ = link.sends();
        messages_from_ = link.messages();
        udp_from_ = link.udp_first();
    }
    bool active() const { return active_; }

    // Each race frame run.
    void frame(const rt::M2Board &board, uint64_t now_ns) {
        if (!active_) return;
        const int rx = board.comm_ram()[0x21c0 + 0x0a];
        if (previous_ >= 0) ++steps_[std::min(3, (rx - previous_) & 0xff)];
        previous_ = rx;
        ++frames_;
        last_ns_ = now_ns;
    }
    // A loop of the app's while the menu shows over the race.
    void menu(uint64_t ns) {
        if (active_) menu_ns_ += ns;
    }

    // The race over (or the room left): one line in the log.
    void end(const RelayLink &link, const rt::M2Board &board, const char *why) {
        if (!active_) return;
        active_ = false;
        const double seconds = double(last_ns_ - from_ns_) / 1e9;
        const uint64_t total = steps_[0] + steps_[1] + steps_[2] + steps_[3];
        const double pct = total ? 100.0 / double(total) : 0.0;
        const rt::CommBoard *cb = board.comm_board();
        std::printf("daytona: [%s] online race %s after %.0f s: game speed %.1f%%, the other car's data %.1f%% smooth, "
                    "%.1f%% no new data, %.1f%% skips; link reserve %d (%d changes); %.1f sends/s, %.1f messages/s, "
                    "%" PRIu64 " first by UDP; %.0f s in the menu\n",
                    online::utc_now().c_str(), why, seconds, seconds > 1 ? 100.0 * double(frames_) / (seconds * 57.524) : 0.0, steps_[1] * pct,
                    steps_[0] * pct, (steps_[2] + steps_[3]) * pct, cb ? cb->reserve() : 0, cb ? cb->reserve_changes() : 0,
                    seconds > 1 ? double(link.sends() - sends_from_) / seconds : 0.0,
                    seconds > 1 ? double(link.messages() - messages_from_) / seconds : 0.0, link.udp_first() - udp_from_,
                    double(menu_ns_) / 1e9);
        // and each player's latency to the server at its end
        const auto &st = link.status();
        std::string players;
        for (size_t i = 0; i < st.names.size(); ++i) {
            if (!players.empty()) players += ", ";
            players += (st.names[i].empty() ? "player " + std::to_string(i + 1) : st.names[i]) + " " +
                       (i < st.latency.size() && st.latency[i] >= 0 ? std::to_string(st.latency[i]) + " ms" : "--");
        }
        std::printf("daytona: online race players (latency to the server): %s\n", players.c_str());
        std::fflush(stdout);
    }

private:
    bool active_ = false;
    uint64_t steps_[4] = {};
    int previous_ = -1;
    uint64_t frames_ = 0, from_ns_ = 0, last_ns_ = 0, menu_ns_ = 0;
    uint64_t sends_from_ = 0, messages_from_ = 0, udp_from_ = 0;
};

} // namespace app
