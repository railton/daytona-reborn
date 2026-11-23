// Online play: the communication board's transport through a daytona-relay
// server instead of TCP to the next cabinet (link_socket.h). One outgoing
// connection carries both directions: what this game sends goes to the
// server, which passes it to the next player of the room's ring; what the
// previous player sent comes back. No port to open on the player's router.
//
// The lines count as open (rx_open/tx_open) only while the room is complete,
// as a cabinet's cables are only joined when every cabinet is there; when the
// room changes, the ring starts again (a new epoch) and old data is dropped.
// Everything is non-blocking and polled from the main thread; only the name
// lookup and the connect run on a thread of their own.
#pragma once

#include "app/online.h"
#include "runtime/comm_board.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {

class RelayLink : public rt::LinkTransport {
public:
    enum class State { Connecting, Waiting, Ready, Failed };

    // server: host:port (online::normalize_server).
    RelayLink(const std::string &server, const online::Hello &hello);
    ~RelayLink() override;
    RelayLink(const RelayLink &) = delete;
    RelayLink &operator=(const RelayLink &) = delete;

    void poll() override;
    bool rx_open() const override { return state_ == State::Ready; }
    bool tx_open() const override { return state_ == State::Ready; }
    int read(uint8_t *buf, int max) override;
    bool write(const uint8_t *buf, int n) override;

    State state() const { return state_; }
    const std::string &error() const { return error_; }
    const online::Status &status() const { return status_; } // the last the server sent
    uint32_t epoch() const { return epoch_; }                 // changes with every new ring
    const std::string &server() const { return server_; }
    const std::string &room() const { return room_; }
    // The games' own coordination (rt::online::CourseSync), outside the
    // arcade's link data: to every other player of the room; what they sent
    // since the last call. Only while the room is complete, this ring's only.
    void send_control(const std::string &text);
    // The room's owner starts a race with everyone in the room (Status::owner).
    void begin();
    std::vector<std::string> take_controls();

private:
    struct Connect; // the connect thread's result, shared with it
    std::shared_ptr<Connect> connect_;
    long long sock_ = -1;
    State state_ = State::Connecting;
    std::string error_, server_, room_, hello_;
    online::Status status_;
    uint32_t epoch_ = 0;
    std::string out_, in_, inbox_; // to the server; from it (unparsed); for the game
    std::vector<std::string> controls_;
    size_t inbox_pos_ = 0;
    uint64_t last_rx_ms_ = 0, last_tx_ms_ = 0;
    void fail(const std::string &why);
    void flush();
    void receive();
    void frame(uint8_t type, uint32_t epoch, const std::string &payload);
};

} // namespace app
