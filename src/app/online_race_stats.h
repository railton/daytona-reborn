// An online race's own measurements, for the log (daytona.log), so a
// player's log says what a race was like when something went wrong, as
// m2run --relay says it in tests:
//
//   - how the previous car's data moved each frame (byte 0x0a of the block
//     received at 0x21c0, that cabinet's tick counter): 1 is smooth, 0 a
//     frame with no new data (the car stalls), 2+ a skip (it jumps);
//   - the same for every other car of the room (slot k at 0x21c0 + k * 0x1c0
//     holds the car k + 1 places behind in the ring: the relay composes all
//     but the one just ahead), with its longest stall and the worst car named:
//     the car just ahead alone said nothing of the others (0.22's lesson);
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
        for (int k = 0; k < kCars; ++k) { // every other car: its steps, its stalls in frames
            const int tick = board.comm_ram()[0x21c0 + k * 0x1c0 + 0x0a];
            if (car_previous_[k] >= 0) {
                const int step = (tick - car_previous_[k]) & 0xff;
                ++car_steps_[k][std::min(3, step)];
                car_stall_[k] = step ? 0 : car_stall_[k] + 1;
                car_longest_[k] = std::max(car_longest_[k], car_stall_[k]);
            }
            car_previous_[k] = tick;
        }
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
        // each other car, by its place behind this one in the ring (names in ring order), and the worst
        if (const int others = st.size - 1; others > 1 && st.position >= 1) {
            std::string cars, worst_name;
            double worst = 101.0;
            for (int k = 0; k < others && k < kCars; ++k) {
                const uint64_t n = car_steps_[k][0] + car_steps_[k][1] + car_steps_[k][2] + car_steps_[k][3];
                if (!n) continue;
                const double one = 100.0 * double(car_steps_[k][1]) / double(n);
                const int place = ((st.position - 2 - k) % st.size + st.size) % st.size; // 0-based ring place
                const std::string name = size_t(place) < st.names.size() && !st.names[size_t(place)].empty()
                                             ? st.names[size_t(place)]
                                             : "player " + std::to_string(place + 1);
                char part[160];
                std::snprintf(part, sizeof part, "%s%s %.1f%% smooth (%.1f%% none, %.1f%% skips), longest stall %d ms",
                              cars.empty() ? "" : "; ", name.c_str(), one, 100.0 * double(car_steps_[k][0]) / double(n),
                              100.0 * double(car_steps_[k][2] + car_steps_[k][3]) / double(n),
                              int(car_longest_[k] * 1000.0 / 57.524 + 0.5));
                cars += part;
                if (one < worst) worst = one, worst_name = name;
            }
            if (!cars.empty())
                std::printf("daytona: online race, each other car: %s; the worst: %s, %.1f%%\n", cars.c_str(), worst_name.c_str(),
                            worst);
        }
        std::fflush(stdout);
    }

private:
    bool active_ = false;
    uint64_t steps_[4] = {};
    int previous_ = -1;
    static constexpr int kCars = 7; // the other cars of a room of 8
    uint64_t car_steps_[kCars][4] = {};
    int car_previous_[kCars] = {-1, -1, -1, -1, -1, -1, -1};
    int car_stall_[kCars] = {}, car_longest_[kCars] = {}; // frames without new data in a row, now and the longest
    uint64_t frames_ = 0, from_ns_ = 0, last_ns_ = 0, menu_ns_ = 0;
    uint64_t sends_from_ = 0, messages_from_ = 0, udp_from_ = 0;
};

} // namespace app
