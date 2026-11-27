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

#include "app/link_codec.h"
#include "app/online.h"
#include "runtime/comm_board.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace app {

class RelayLink : public rt::LinkTransport {
public:
    // Info: the server answered Hello::Op::Info (info()); the connection is closed.
    enum class State { Connecting, Waiting, Ready, Failed, Info };

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
    void flush_now() override {
        if (state_ == State::Ready && !reconnecting_) flush();
    }

    State state() const { return state_; }
    const std::string &error() const { return error_; }
    // The server's refusal code (proto.go: "password", "full"...); empty for any other failure.
    const std::string &error_code() const { return error_code_; }
    // After a "version" or "game_version" refusal: who has to update (online::refusal_outdated).
    online::Outdated outdated() const { return outdated_; }
    const online::Status &status() const { return status_; } // the last the server sent
    const std::vector<std::pair<std::string, std::string>> &info() const { return info_; }
    // diagnostics: send() calls made, game messages (D frames) sent
    uint64_t sends() const { return sends_; }
    uint64_t udp_first() const { return udp_first_; } // messages UDP brought before TCP
    uint64_t messages() const { return messages_; }
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
    std::string error_, error_code_, server_, room_, hello_;
    online::Outdated outdated_ = online::Outdated::None;
    online::Status status_;
    std::vector<std::pair<std::string, std::string>> info_;
    uint32_t epoch_ = 0;
    std::string out_, in_, inbox_; // to the server; from it (unparsed); for the game
    std::vector<std::string> controls_;
    online::LinkCodec tx_codec_, rx_codec_; // this ring's wire frames, each way (app/link_codec.h)
    size_t inbox_pos_ = 0;
    uint64_t last_rx_ms_ = 0, last_tx_ms_ = 0;
    uint64_t sends_ = 0, messages_ = 0;
    bool coalesce_ = true; // game messages sent together, when the link says (flush_now; M2_PROTO_COALESCE=0: each at once)
    void fail(const std::string &why);
    // The connection dropped mid-race: come back with the key if the server
    // keeps places (Status::resume), the game's link kept open meanwhile.
    void lost(const std::string &why);
    void start_connect();
    online::Hello hello_struct_;
    // Numbered game data, by TCP and UDP (the relay's udp.go): each message
    // in its TCP frame and in a datagram repeating the last ones; received
    // once, in order, from whichever comes first.
    bool udp_ = false;
    long long udp_sock_ = -1;
    uint32_t tx_seq_ = 0, rx_next_ = 0;
    std::vector<std::string> tx_recent_; // numbered: seq(4) + message, sent by UDP, for repetition
    std::vector<std::string> udp_pend_;  // numbered, for the next datagram (udp_flush)
    int udp_repeat_ = 2;                 // earlier messages repeated in each datagram (M2_PROTO_UDP_REPEAT)
    void udp_flush();
    std::map<uint32_t, std::string> rx_early_;
    uint64_t udp_hello_ms_ = 0;
    uint64_t udp_received_ = 0, udp_first_ = 0; // diagnostics: datagram messages, delivered first by UDP
    void udp_open();
    void udp_close();
    void udp_poll();
    void numbered(uint32_t epoch, uint32_t seq, const std::string &message, bool by_udp);
    void deliver(const std::string &message);
    bool reconnecting_ = false, may_reconnect_ = true;
    uint64_t reconnect_until_ms_ = 0, next_try_ms_ = 0;
    void refused(const std::string &payload); // an E frame: its code, then fail
    void flush();
    void receive();
    void frame(uint8_t type, uint32_t epoch, const std::string &payload);
};

} // namespace app
