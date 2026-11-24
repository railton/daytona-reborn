#include "app/online.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sstream>

namespace app::online {

namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford: no I L O U
constexpr char kPrefix[] = "DTN1";

int symbol(char c) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

std::string base32(const std::string &bytes) {
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char b : bytes) {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 31];
    return out;
}

std::optional<std::string> unbase32(const std::string &text) {
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (char c : text) {
        const int v = symbol(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 5) | unsigned(v);
        bits += 5;
        if (bits >= 8) {
            out += char((buffer >> (bits - 8)) & 0xff);
            bits -= 8;
        }
    }
    return out;
}

// CRC-16/XMODEM: a cut or mistyped invite code is refused, not half-read
uint16_t check(const std::string &bytes) {
    uint16_t value = 0;
    for (unsigned char b : bytes) {
        value ^= uint16_t(b << 8);
        for (int bit = 0; bit < 8; ++bit) value = uint16_t((value << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
    }
    return value;
}

bool printable_field(const std::string &s) {
    return std::none_of(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\0'; });
}
} // namespace

std::string encode_invite(const Invite &invite) {
    std::string bytes = invite.server + "\n" + invite.room + "\n" + invite.password;
    const uint16_t sum = check(bytes);
    bytes += char(sum >> 8);
    bytes += char(sum);
    const std::string body = base32(bytes);
    std::string out = kPrefix;
    for (size_t i = 0; i < body.size(); i += 5) out += "-" + body.substr(i, 5);
    return out;
}

std::optional<Invite> decode_invite(const std::string &code) {
    std::string text;
    for (char c : code)
        if (std::isalnum(static_cast<unsigned char>(c))) text += c;
    if (text.size() <= 4) return std::nullopt;
    std::string prefix = text.substr(0, 4);
    for (auto &c : prefix) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (prefix != kPrefix) return std::nullopt;
    auto bytes = unbase32(text.substr(4));
    if (!bytes || bytes->size() < 2) return std::nullopt;
    const uint16_t sum = uint16_t(uint8_t((*bytes)[bytes->size() - 2]) << 8 | uint8_t(bytes->back()));
    bytes->resize(bytes->size() - 2);
    if (check(*bytes) != sum) return std::nullopt;
    const auto a = bytes->find('\n');
    const auto b = a == std::string::npos ? a : bytes->find('\n', a + 1);
    if (b == std::string::npos) return std::nullopt;
    Invite invite{bytes->substr(0, a), bytes->substr(a + 1, b - a - 1), bytes->substr(b + 1)};
    if (!normalize_server(invite.server) || !valid_room(invite.room)) return std::nullopt;
    return invite;
}

std::string random_room() {
    static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::uniform_int_distribution<int> pick(0, int(sizeof chars) - 2);
    std::string room;
    for (int i = 0; i < 8; ++i) room += chars[pick(rd)];
    return room;
}

std::optional<std::string> normalize_server(const std::string &server) {
    std::string s;
    for (char c : server)
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty() || !printable_field(s)) return std::nullopt;
    std::string host = s, port;
    if (s.front() == '[') { // [IPv6]:port
        const auto close = s.find(']');
        if (close == std::string::npos) return std::nullopt;
        host = s.substr(0, close + 1);
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') return std::nullopt;
            port = s.substr(close + 2);
        }
    } else if (std::count(s.begin(), s.end(), ':') == 1) {
        const auto colon = s.find(':');
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
    } else if (std::count(s.begin(), s.end(), ':') > 1) {
        host = "[" + s + "]"; // a bare IPv6 address
    }
    if (host.empty() || host == "[]") return std::nullopt;
    int p = kDefaultPort;
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return std::nullopt;
        p = std::atoi(port.c_str());
        if (p < 1 || p > 65535) return std::nullopt;
    }
    return host + ":" + std::to_string(p);
}

std::string clean_name(const std::string &name) {
    // UTF-8 aware: drop control bytes, keep at most kMaxName characters
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < name.size() && chars < kMaxName;) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (c < 0x20 || c == 0x7f || i + len > name.size()) { ++i; continue; }
        out.append(name, i, len);
        i += len;
        ++chars;
    }
    const auto first = out.find_first_not_of(' '), last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
}

bool valid_room(const std::string &room) {
    return !room.empty() && room.size() <= 32 && std::all_of(room.begin(), room.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string percent_encode(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

std::string percent_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string hello_line(const Hello &hello) {
    std::string line = "DAYTONA-RELAY 3 op=";
    line += hello.op == Hello::Op::Create ? "create" : hello.op == Hello::Op::Join ? "join" : "info";
    if (hello.op != Hello::Op::Info) line += " room=" + percent_encode(hello.room);
    const std::string name = clean_name(hello.name);
    if (!name.empty()) line += " name=" + percent_encode(name);
    if (!hello.password.empty()) line += " pass=" + percent_encode(hello.password);
    line += " v=" + std::to_string(kOnlineVersion);
    if (hello.op == Hello::Op::Create && !hello.settings.empty()) line += " set=" + percent_encode(hello.settings);
    return line + "\n";
}

std::vector<std::pair<std::string, std::string>> parse_info(const std::string &payload) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(payload);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos && eq > 0) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

std::optional<Status> parse_status(const std::string &payload) {
    std::istringstream in(payload);
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    Status s;
    int ready = 0;
    if (std::sscanf(line.c_str(), "joined=%d size=%d pos=%d ready=%d", &s.joined, &s.size, &s.position, &ready) != 4)
        return std::nullopt;
    s.ready = ready == 1;
    while (std::getline(in, line)) {
        if (line.rfind("eeprom=", 0) == 0) {
            std::istringstream pairs(line.substr(7));
            std::string pair;
            while (std::getline(pairs, pair, ',')) {
                unsigned offset = 0, value = 0;
                if (std::sscanf(pair.c_str(), "%x:%x", &offset, &value) == 2 && offset < 128 && value < 256 &&
                    rt::online::allowed(uint8_t(offset)))
                    s.eeprom.push_back({uint8_t(offset), uint8_t(value)});
            }
        } else if (line.rfind("name=", 0) == 0) {
            s.names.push_back(clean_name(percent_decode(line.substr(5))));
        } else if (line == "start=anyone") {
            s.any_start = true;
        } else if (line == "owner=1") {
            s.owner = true;
        } else if (line == "racing=1") {
            s.racing = true;
        } else if (line.rfind("me=", 0) == 0) {
            s.me = std::atoi(line.c_str() + 3);
        } else if (line.rfind("waiting=", 0) == 0) {
            s.waiting.push_back(clean_name(percent_decode(line.substr(8))));
        }
    }
    return s;
}

void append_frame(std::string &out, uint8_t type, uint32_t epoch, const uint8_t *payload, size_t n) {
    out += char(type);
    out += char(epoch >> 24);
    out += char(epoch >> 16);
    out += char(epoch >> 8);
    out += char(epoch);
    out += char(n >> 8);
    out += char(n);
    if (n) out.append(reinterpret_cast<const char *>(payload), n);
}

} // namespace app::online
