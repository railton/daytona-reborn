#include "app/relay_link.h"
#include "app/i18n_mark.h"

#include <atomic>
#include <chrono>
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
    if (code == "size") return TXR("This room is not for that number of players.");
    if (code == "password") return TXR("Wrong server password.");
    if (code == "server_full") return TXR("The server is full: too many rooms open.");
    if (code == "too_many") return TXR("Too many connections to the server from your address.");
    if (code == "version") return TXR("This server is not compatible with this version of the game.");
    if (code == "room") return TXR("Invalid room code.");
    if (code == "players") return TXR("Invalid number of players: 2 to 8.");
    if (code == "settings") return TXR("The server refused the room's settings.");
    if (code == "room_kind") return TXR("This room belongs to the daytona-link helper, not to the game's Online mode.");
    if (code == "game_version")
        return TXR("This room was created by another version of the game: every player needs the same version.");
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
    : server_(server), room_(hello.room), hello_(online::hello_line(hello)) {
    net_init();
    connect_ = std::make_shared<Connect>();
    std::thread([c = connect_, server] {
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

RelayLink::~RelayLink() {
    if (connect_) {
        std::lock_guard<std::mutex> lock(connect_->mu);
        connect_->abandoned = true;
        if (connect_->done && connect_->sock >= 0 && connect_->sock != sock_) close_socket(connect_->sock);
    }
    if (sock_ >= 0) close_socket(sock_);
}

void RelayLink::fail(const std::string &why) {
    if (state_ == State::Failed) return;
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
    fail(server_error(payload));
}

void RelayLink::poll() {
    if (state_ == State::Failed || state_ == State::Info) return;
    const uint64_t now = now_ms();
    if (sock_ < 0) {
        std::lock_guard<std::mutex> lock(connect_->mu);
        if (!connect_->done) return;
        if (connect_->sock < 0) {
            fail(connect_->error);
            return;
        }
        sock_ = connect_->sock;
        connect_->sock = -1;
        out_ = hello_ + out_;
        last_rx_ms_ = last_tx_ms_ = now;
    }
    if (now - last_tx_ms_ >= kPingMs) online::append_frame(out_, online::kPing, 0, nullptr, 0);
    flush();
    if (state_ != State::Failed) receive();
    if (state_ != State::Failed && now_ms() - last_rx_ms_ > kTimeoutMs) fail(TXR("The server stopped answering."));
}

void RelayLink::flush() {
    while (!out_.empty()) {
        const auto sent = ::send(native_socket(sock_), out_.data(), int(std::min<size_t>(out_.size(), 1 << 16)), kNoSignal);
        if (sent > 0) {
            out_.erase(0, size_t(sent));
            last_tx_ms_ = now_ms();
            continue;
        }
        if (sent < 0 && would_block(last_error())) break;
        fail(TXR("Connection to the server lost."));
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
        fail(TXR("Connection to the server lost."));
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
            }
            epoch_ = epoch;
            state_ = s->ready ? State::Ready : State::Waiting;
        }
        break;
    case online::kData: // one compressed wire frame of the previous player's
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
bool RelayLink::write(const uint8_t *buf, int n) {
    if (state_ != State::Ready) return false;
    if (n <= 0 || size_t(n) > online::LinkCodec::kMaxFrame) return n == 0;
    std::string message;
    tx_codec_.encode(buf, size_t(n), message);
    online::append_frame(out_, online::kData, epoch_, reinterpret_cast<const uint8_t *>(message.data()), message.size());
    flush();
    return state_ == State::Ready;
}

} // namespace app
