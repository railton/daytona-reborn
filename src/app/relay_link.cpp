#include "app/relay_link.h"
#include "app/i18n_mark.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using native_socket = SOCKET;
namespace {
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
void close_socket(long long s) { closesocket(native_socket(s)); }
bool set_nonblocking(long long s, bool on) {
    u_long v = on ? 1 : 0;
    return ioctlsocket(native_socket(s), FIONBIO, &v) == 0;
}
struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
void net_init() { static WsaInit init; }
constexpr int kNoSignal = 0;
bool valid(native_socket s) { return s != INVALID_SOCKET; }
} // namespace
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using native_socket = int;
namespace {
int last_error() { return errno; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY; }
void close_socket(long long s) { ::close(native_socket(s)); }
bool set_nonblocking(long long s, bool on) {
    const int flags = fcntl(native_socket(s), F_GETFL, 0);
    return flags >= 0 && fcntl(native_socket(s), F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
}
void net_init() {}
#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;
#endif
bool valid(native_socket s) { return s >= 0; }
} // namespace
#endif

namespace app {

namespace {
constexpr uint64_t kPingMs = 5000, kTimeoutMs = 30000, kConnectTimeoutMs = 10000;
constexpr size_t kMaxQueued = 4 << 20; // bytes waiting either way before giving up

// A refusal from the server: "code\ntext". The player reads this game's own
// text for the code, in their language; the server's text only for a code
// this version does not know.
std::string server_error(const std::string &payload) {
    const auto nl = payload.find('\n');
    const std::string code = nl == std::string::npos ? std::string() : payload.substr(0, nl);
    const std::string text = nl == std::string::npos ? payload : payload.substr(nl + 1);
    if (code == "not_found") return TXR("This room does not exist: is the code complete? Is its creator still connected?");
    if (code == "exists") return TXR("This room already exists.");
    if (code == "full") return TXR("This room is full.");
    if (code == "password") return TXR("Wrong server password.");
    if (code == "server_full") return TXR("The server is full: too many rooms open.");
    if (code == "too_many") return TXR("Too many connections to the server from your address.");
    if (code == "version") {
        const auto who = online::refusal_outdated(payload);
        if (who == online::Outdated::Game)
            return TXR("This server runs a newer version of the Online mode: update your game (git pull, or download the "
                       "source again) and build it again.");
        if (who == online::Outdated::Server)
            return TXR("This server runs an older version of the Online mode: whoever runs it needs to update it.");
        return TXR("This server is not compatible with this version of the game.");
    }
    if (code == "room") return TXR("Invalid room code.");
    if (code == "settings") return TXR("The server refused the room's settings.");
    if (code == "too_many_rooms") return TXR("Too many rooms open from your address.");
    if (code == "slow_down") return TXR("Too many failed attempts: wait a minute and try again.");
    if (code == "idle") return TXR("The room was closed: nobody raced in it for a long time.");
    if (code == "flood") return TXR("Disconnected by the server: your game sent too much data.");
    if (code == "game_version") {
        const auto who = online::refusal_outdated(payload);
        if (who == online::Outdated::Game)
            return TXR("This room was created by a newer version of the game: update yours (git pull, or download the "
                       "source again, and build it again) to join it.");
        if (who == online::Outdated::Room)
            return TXR("This room was created by an older version of the game: its creator needs to update to the latest "
                       "version.");
        return TXR("This room was created by another version of the game: every player needs the same version.");
    }
    return std::string(TXR("The server refused the connection: ")) + text;
}

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool split_host_port(const std::string &s, std::string &host, std::string &port) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    host = s.substr(0, colon);
    port = s.substr(colon + 1);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return !host.empty() && !port.empty();
}

// Blocking: the name lookup, then each address with a time limit.
long long connect_to(const std::string &server, std::string &error) {
    std::string host, port;
    if (!split_host_port(server, host, port)) {
        error = TXR("Invalid server address.");
        return -1;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        error = std::string(TXR("Server not found: ")) + host;
        return -1;
    }
    error = std::string(TXR("Cannot connect to the server (is it running? is its port open?): ")) + server;
    for (addrinfo *a = res; a; a = a->ai_next) {
        native_socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (!valid(s)) continue;
        set_nonblocking(s, true);
        int r = ::connect(s, a->ai_addr, socklen_t(a->ai_addrlen));
        if (r != 0 && would_block(last_error())) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            timeval tv{long(kConnectTimeoutMs / 1000), 0};
            if (::select(int(s + 1), nullptr, &w, nullptr, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
                r = err == 0 ? 0 : -1;
            } else {
                r = -1;
            }
        }
        if (r == 0) {
            freeaddrinfo(res);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            error.clear();
            return static_cast<long long>(s);
        }
        close_socket(static_cast<long long>(s));
    }
    freeaddrinfo(res);
    return -1;
}
} // namespace

struct RelayLink::Connect {
    std::mutex mu;
    bool done = false, abandoned = false;
    long long sock = -1;
    std::string error;
};

RelayLink::RelayLink(const std::string &server, const online::Hello &hello)
    : server_(server), room_(hello.room), hello_(online::hello_line(hello)), hello_struct_(hello) {
    // all on by default; each switch, set to 0, turns one off (diagnostics,
    // docs/ONLINE-PROTOCOL.md, "Switches for diagnostics")
    if (const char *v = std::getenv("M2_PROTO_COALESCE")) coalesce_ = *v != '0';
    if (const char *v = std::getenv("M2_PROTO_RECONNECT")) may_reconnect_ = *v != '0';
    if (const char *v = std::getenv("M2_PROTO_UDP_REPEAT")) udp_repeat_ = std::max(0, std::atoi(v));
    const char *u = std::getenv("M2_PROTO_UDP");
    if (!(u && *u == '0') && hello.op != online::Hello::Op::Info) {
        udp_ = true;
        hello_struct_.udp = true;
        hello_ = online::hello_line(hello_struct_);
    }
    net_init();
    start_connect();
}

void RelayLink::start_connect() {
    if (connect_) {
        std::lock_guard<std::mutex> lock(connect_->mu);
        connect_->abandoned = true;
        if (connect_->done && connect_->sock >= 0) close_socket(connect_->sock), connect_->sock = -1;
    }
    connect_ = std::make_shared<Connect>();
    std::thread([c = connect_, server = server_] {
        std::string error;
        const long long s = connect_to(server, error);
        std::lock_guard<std::mutex> lock(c->mu);
        if (c->abandoned) {
            if (s >= 0) close_socket(s);
            return;
        }
        c->sock = s;
        c->error = error;
        c->done = true;
    }).detach();
}

void RelayLink::lost(const std::string &why) {
    const uint64_t now = now_ms();
    if (!reconnecting_) {
        if (!(may_reconnect_ && state_ == State::Ready && status_.resume && !status_.key.empty())) {
            fail(why);
            return;
        }
        reconnecting_ = true;
        reconnect_until_ms_ = now + 10000; // the server keeps the place 10 s
        std::printf("relay: [%s] connection lost (%s), coming back into the race\n", online::utc_now().c_str(), why.c_str());
    } else if (now > reconnect_until_ms_) {
        reconnecting_ = false;
        fail(why);
        return;
    }
    if (sock_ >= 0) close_socket(sock_);
    sock_ = -1;
    out_.clear();
    in_.clear();
    inbox_.clear();
    inbox_pos_ = 0;
    tx_codec_.reset(); // the relay starts its side of both streams again (star mode)
    rx_codec_.reset();
    tx_seq_ = rx_next_ = 0; // and its numbers (a new client there)
    tx_recent_.clear();
    udp_pend_.clear();
    rx_early_.clear();
    udp_close();
    online::Hello back = hello_struct_;
    back.op = online::Hello::Op::Rejoin;
    back.key = status_.key;
    hello_ = online::hello_line(back);
    next_try_ms_ = now + 300;
    connect_.reset();
}

RelayLink::~RelayLink() {
    if (connect_) {
        std::lock_guard<std::mutex> lock(connect_->mu);
        connect_->abandoned = true;
        if (connect_->done && connect_->sock >= 0 && connect_->sock != sock_) close_socket(connect_->sock);
    }
    if (sock_ >= 0) close_socket(sock_);
    udp_close();
}

void RelayLink::fail(const std::string &why) {
    if (state_ == State::Failed) return;
    if (hello_struct_.op != online::Hello::Op::Info) // the log (a server's probe failing is no news)
        std::printf("relay: [%s] %s %s: %s\n", online::utc_now().c_str(),
                    state_ == State::Ready ? "connection to the race ended," : "connection failed,", server_.c_str(), why.c_str());
    state_ = State::Failed;
    error_ = why;
    if (sock_ >= 0) close_socket(sock_);
    sock_ = -1;
    inbox_.clear();
    inbox_pos_ = 0;
}

void RelayLink::refused(const std::string &payload) {
    const auto nl = payload.find('\n');
    error_code_ = nl == std::string::npos ? std::string() : payload.substr(0, nl);
    outdated_ = online::refusal_outdated(payload);
    fail(server_error(payload));
}

void RelayLink::poll() {
    if (state_ == State::Failed || state_ == State::Info) return;
    const uint64_t now = now_ms();
    if (sock_ < 0) {
        if (reconnecting_) {
            if (now > reconnect_until_ms_) {
                lost(TXR("Connection to the server lost."));
                return;
            }
            if (!connect_) {
                if (now < next_try_ms_) return;
                start_connect();
            }
        }
        // a reference of our own: connect_.reset() below must not destroy the
        // mutex this lock holds (the connect thread may have let go of it)
        const std::shared_ptr<Connect> attempt = connect_;
        std::lock_guard<std::mutex> lock(attempt->mu);
        if (!connect_->done) return;
        if (connect_->sock < 0) {
            if (reconnecting_) { // try again shortly, until the deadline
                connect_.reset();
                next_try_ms_ = now + 500;
                return;
            }
            fail(connect_->error);
            return;
        }
        sock_ = connect_->sock;
        connect_->sock = -1;
        out_ = hello_ + out_;
        last_rx_ms_ = last_tx_ms_ = now;
        if (udp_) udp_open();
    }
    if (udp_) udp_poll();
    if (now - last_tx_ms_ >= kPingMs) online::append_frame(out_, online::kPing, 0, nullptr, 0);
    flush();
    if (state_ != State::Failed) receive();
    // the server pings every 2 s: in a race that can come back, 5 s of silence is a drop
    const uint64_t silence = reconnecting_ || (state_ == State::Ready && status_.resume) ? 5000 : kTimeoutMs;
    if (state_ != State::Failed && sock_ >= 0 && now_ms() - last_rx_ms_ > silence) lost(TXR("The server stopped answering."));
}

// The messages written since the last flush (a frame's block and token)
// in one datagram, newest first, with the udp_repeat_ messages before them.
void RelayLink::udp_flush() {
    if (udp_pend_.empty()) return;
    std::string dg;
    auto start = [&] {
        dg = "d";
        for (int i = 0; i < 4; ++i) dg += char(uint8_t(epoch_ >> (24 - 8 * i)));
        dg += '\0';
    };
    auto add = [&](const std::string &m) { // m: seq(4) + message
        if (dg.size() + 2 + m.size() > 1200 || uint8_t(dg[5]) == 255) return false;
        dg.append(m, 0, 4);
        dg += char(uint8_t((m.size() - 4) >> 8));
        dg += char(uint8_t(m.size() - 4));
        dg.append(m, 4, std::string::npos);
        dg[5] = char(uint8_t(dg[5]) + 1);
        return true;
    };
    auto send = [&] {
        if (udp_sock_ >= 0 && dg[5] != 0) ::send(native_socket(udp_sock_), dg.data(), int(dg.size()), kNoSignal);
    };
    start();
    for (auto it = udp_pend_.rbegin(); it != udp_pend_.rend(); ++it)
        if (!add(*it)) {
            send();
            start();
            add(*it); // larger than a datagram: TCP alone carries it
        }
    for (auto it = tx_recent_.rbegin(); it != tx_recent_.rend() && add(*it); ++it) {}
    send();
    for (auto &m : udp_pend_) tx_recent_.push_back(std::move(m));
    udp_pend_.clear();
    if (tx_recent_.size() > size_t(udp_repeat_)) tx_recent_.erase(tx_recent_.begin(), tx_recent_.end() - udp_repeat_);
}

void RelayLink::flush() {
    if (sock_ < 0) return; // reconnecting: sent once back
    if (udp_) udp_flush();
    while (!out_.empty()) {
        const auto sent = ::send(native_socket(sock_), out_.data(), int(std::min<size_t>(out_.size(), 1 << 16)), kNoSignal);
        if (sent > 0) {
            ++sends_;
            out_.erase(0, size_t(sent));
            last_tx_ms_ = now_ms();
            continue;
        }
        if (sent < 0 && would_block(last_error())) break;
        lost(TXR("Connection to the server lost."));
        return;
    }
    if (out_.size() > kMaxQueued) fail(TXR("The connection to the server is too slow."));
}

void RelayLink::receive() {
    char buf[16384];
    for (;;) {
        const auto n = ::recv(native_socket(sock_), buf, int(sizeof buf), 0);
        if (n > 0) {
            in_.append(buf, size_t(n));
            last_rx_ms_ = now_ms();
            continue;
        }
        if (n < 0 && would_block(last_error())) break;
        // closed: an error frame may be what came last
        size_t pos = 0;
        while (in_.size() - pos >= online::kHeader) {
            const size_t len = size_t(uint8_t(in_[pos + 5])) << 8 | uint8_t(in_[pos + 6]);
            if (in_.size() - pos < online::kHeader + len) break;
            if (uint8_t(in_[pos]) == online::kError) {
                refused(in_.substr(pos + online::kHeader, len));
                return;
            }
            if (uint8_t(in_[pos]) == online::kInfo) { // the answer to op=info, then the server closes
                frame(online::kInfo, 0, in_.substr(pos + online::kHeader, len));
                return;
            }
            pos += online::kHeader + len;
        }
        lost(TXR("Connection to the server lost."));
        return;
    }
    size_t pos = 0;
    while (in_.size() - pos >= online::kHeader && state_ != State::Failed && state_ != State::Info) {
        const auto *p = reinterpret_cast<const uint8_t *>(in_.data() + pos);
        const size_t len = size_t(p[5]) << 8 | p[6];
        if (len > online::kMaxPayload) {
            fail(TXR("Invalid answer from the server."));
            return;
        }
        if (in_.size() - pos < online::kHeader + len) break;
        const uint32_t epoch = uint32_t(p[1]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4];
        frame(p[0], epoch, in_.substr(pos + online::kHeader, len));
        // the frame may have lost the connection (a ping's answer flushed, the send failed):
        // lost() emptied in_ for the next one, so nothing more of it is read
        if (sock_ < 0) return;
        pos += online::kHeader + len;
    }
    if (state_ != State::Failed) in_.erase(0, pos);
    if (state_ == State::Info && sock_ >= 0) {
        close_socket(sock_);
        sock_ = -1;
    }
}

void RelayLink::frame(uint8_t type, uint32_t epoch, const std::string &payload) {
    switch (type) {
    case online::kError: refused(payload); break;
    case online::kInfo:
        info_ = online::parse_info(payload);
        state_ = State::Info;
        break;
    case online::kStatus:
        if (auto s = online::parse_status(payload)) {
            status_ = *s;
            if (epoch != epoch_) { // a new ring: nothing of the old one carries over
                inbox_.clear();
                inbox_pos_ = 0;
                controls_.clear();
                tx_codec_.reset();
                rx_codec_.reset();
                tx_seq_ = rx_next_ = 0; // numbers start again with the ring
                tx_recent_.clear();
                udp_pend_.clear();
                rx_early_.clear();
            }
            epoch_ = epoch;
            state_ = s->ready ? State::Ready : State::Waiting;
            if (reconnecting_) {
                reconnecting_ = false;
                std::printf("relay: [%s] back in the race\n", online::utc_now().c_str());
            }
        }
        break;
    case online::kData: // one compressed wire frame of the previous player's
        if (udp_) { // numbered: seq(4), then the message
            if (payload.size() >= 4)
                numbered(epoch, uint32_t(uint8_t(payload[0])) << 24 | uint32_t(uint8_t(payload[1])) << 16 |
                                    uint32_t(uint8_t(payload[2])) << 8 | uint8_t(payload[3]),
                         payload.substr(4), false);
            break;
        }
        if (state_ == State::Ready && epoch == epoch_) {
            if (inbox_.size() - inbox_pos_ > kMaxQueued) fail(TXR("The game is not reading the data it receives."));
            else if (!rx_codec_.decode(reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), inbox_))
                fail(TXR("Invalid data from another player: is everyone on the same version of the game?"));
        }
        break;
    case online::kPing: // the server measuring this player's latency: its stamp straight back
        if (!payload.empty()) {
            online::append_frame(out_, online::kPong, epoch, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
            flush();
        }
        break;
    case online::kControl:
        if (state_ == State::Ready && epoch == epoch_ && controls_.size() < 256) controls_.push_back(payload);
        break;
    default: break; // ping
    }
}

void RelayLink::send_control(const std::string &text) {
    if (state_ != State::Ready || text.empty() || text.size() > 256) return;
    online::append_frame(out_, online::kControl, epoch_, reinterpret_cast<const uint8_t *>(text.data()), text.size());
    flush();
}

void RelayLink::begin() {
    if (sock_ < 0 || state_ == State::Failed || !status_.owner) return;
    online::append_frame(out_, online::kBegin, epoch_, nullptr, 0);
    flush();
}

std::vector<std::string> RelayLink::take_controls() {
    std::vector<std::string> out;
    out.swap(controls_);
    return out;
}

int RelayLink::read(uint8_t *buf, int max) {
    if (state_ != State::Ready) return state_ == State::Failed ? -1 : 0;
    const size_t avail = inbox_.size() - inbox_pos_;
    if (avail == 0 || max <= 0) return 0;
    const size_t n = std::min(avail, size_t(max));
    std::memcpy(buf, inbox_.data() + inbox_pos_, n);
    inbox_pos_ += n;
    if (inbox_pos_ == inbox_.size()) inbox_.clear(), inbox_pos_ = 0;
    else if (inbox_pos_ > (1u << 16)) inbox_.erase(0, inbox_pos_), inbox_pos_ = 0;
    return int(n);
}

// One wire frame of the comm board's a call (CommBoard::send_frame): one
// compressed message (app/link_codec.h).
// Numbered game data: the UDP socket, beside the TCP connection to the same
// address and port.
void RelayLink::udp_open() {
    udp_close();
    sockaddr_storage peer{};
    socklen_t len = sizeof peer;
    if (::getpeername(native_socket(sock_), reinterpret_cast<sockaddr *>(&peer), &len) != 0) return;
    native_socket u = ::socket(peer.ss_family, SOCK_DGRAM, 0);
    if (!valid(u)) return;
    set_nonblocking(static_cast<long long>(u), true);
    if (::connect(u, reinterpret_cast<sockaddr *>(&peer), len) != 0) {
        close_socket(static_cast<long long>(u));
        return;
    }
    udp_sock_ = static_cast<long long>(u);
    udp_hello_ms_ = 0;
}

void RelayLink::udp_close() {
    if (udp_sock_ >= 0) close_socket(udp_sock_);
    udp_sock_ = -1;
}

void RelayLink::udp_poll() {
    if (udp_sock_ < 0) return;
    const uint64_t now = now_ms();
    if (!status_.key.empty() && now - udp_hello_ms_ >= 2000) { // who this address is (and the NAT kept open)
        const std::string h = "h" + status_.key;
        ::send(native_socket(udp_sock_), h.data(), int(h.size()), kNoSignal);
        udp_hello_ms_ = now;
    }
    char buf[2048];
    for (;;) {
        const auto n = ::recv(native_socket(udp_sock_), buf, int(sizeof buf), 0);
        if (n <= 0) break;
        if (buf[0] != 'd' || n < 6) continue;
        const auto *p = reinterpret_cast<const uint8_t *>(buf);
        const uint32_t epoch = uint32_t(p[1]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4];
        size_t at = 6;
        for (int i = 0; i < p[5] && at + 6 <= size_t(n); ++i) {
            const uint32_t seq = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
            const size_t l = size_t(p[at + 4]) << 8 | p[at + 5];
            at += 6;
            if (at + l > size_t(n)) break;
            numbered(epoch, seq, std::string(buf + at, l), true);
            at += l;
        }
    }
}

// A numbered message, from TCP or UDP: once, in order.
void RelayLink::numbered(uint32_t epoch, uint32_t seq, const std::string &message, bool by_udp) {
    if (state_ != State::Ready || epoch != epoch_ || reconnecting_) return;
    if (by_udp) ++udp_received_;
    if (seq < rx_next_) return; // had it
    if (seq > rx_next_) {
        if (rx_early_.size() < 64) rx_early_.emplace(seq, message);
        return;
    }
    if (by_udp) ++udp_first_;
    deliver(message);
    ++rx_next_;
    for (auto it = rx_early_.find(rx_next_); it != rx_early_.end() && state_ == State::Ready; it = rx_early_.find(rx_next_)) {
        deliver(it->second);
        rx_early_.erase(it);
        ++rx_next_;
    }
}

void RelayLink::deliver(const std::string &m) {
    if (inbox_.size() - inbox_pos_ > kMaxQueued) fail(TXR("The game is not reading the data it receives."));
    else if (!rx_codec_.decode(reinterpret_cast<const uint8_t *>(m.data()), m.size(), inbox_))
        fail(TXR("Invalid data from another player: is everyone on the same version of the game?"));
}

bool RelayLink::write(const uint8_t *buf, int n) {
    if (state_ != State::Ready) return false;
    if (reconnecting_) return true; // dropped while away: the codecs start again once back
    if (n <= 0 || size_t(n) > online::LinkCodec::kMaxFrame) return n == 0;
    std::string message;
    tx_codec_.encode(buf, size_t(n), message);
    if (udp_) {
        std::string numbered(4, '\0');
        for (int i = 0; i < 4; ++i) numbered[size_t(i)] = char(uint8_t(tx_seq_ >> (24 - 8 * i)));
        ++tx_seq_;
        numbered += message;
        online::append_frame(out_, online::kData, epoch_, reinterpret_cast<const uint8_t *>(numbered.data()), numbered.size());
        if (udp_sock_ >= 0) udp_pend_.push_back(std::move(numbered)); // its datagram goes with the flush (udp_flush)
    } else {
        online::append_frame(out_, online::kData, epoch_, reinterpret_cast<const uint8_t *>(message.data()), message.size());
    }
    ++messages_;
    if (!coalesce_) flush(); // coalesced: at the next poll (the board polls at least once a ms while the game waits)
    return state_ == State::Ready;
}

} // namespace app
