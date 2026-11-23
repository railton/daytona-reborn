// Online play through a daytona-relay server (server/daytona-relay): the
// protocol's text parts and the invite code. No sockets here (relay_link.h),
// so all of it is unit-tested (tests/test_online.cpp).
#pragma once

#include "runtime/online_settings.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app::online {

constexpr int kMinPlayers = 2, kMaxPlayers = 8;
constexpr int kDefaultPort = 7777;
constexpr size_t kMaxName = 20;
// This game's online behaviour (what it sends besides the arcade's own link
// data, how it starts and joins races). The server keeps a room to one
// version, so every player of a room behaves the same. Raise it with any
// such change.
constexpr int kOnlineVersion = 4;

// What a friend needs to join: the server, the room, the server's password.
struct Invite {
    std::string server; // host:port
    std::string room;
    std::string password;
};
// "DTN1-XXXXX-XXXXX-...": Crockford base32 of the three fields and a
// CRC-16, so it can be read aloud, typed in either case, pasted with or
// without the dashes, and a cut or mistyped code is refused.
std::string encode_invite(const Invite &invite);
std::optional<Invite> decode_invite(const std::string &code);

// A new room's code: 8 random lowercase letters and digits.
std::string random_room();
// host or host:port (IPv6 in brackets) -> host:port, kDefaultPort when none.
std::optional<std::string> normalize_server(const std::string &server);
std::string clean_name(const std::string &name);
bool valid_room(const std::string &room);

struct Hello {
    enum class Op { Create, Join } op = Op::Join;
    std::string room;
    std::string name, password;
};
std::string hello_line(const Hello &hello); // with the trailing newline

// A room takes up to kMaxPlayers; its owner (the creator, or the first in a
// room made on the web) starts each race with everyone then in the room.
struct Status {
    int joined = 0;   // players in the room
    int size = 0;     // players in the race's ring; 0 while no race is formed
    int position = 0; // this player's place in the ring (1 = MASTER); 0: not in it
    bool ready = false; // this player is in a formed ring: the game's link opens
    std::vector<rt::online::Setting> eeprom;
    std::vector<std::string> names; // ring order
    bool any_start = false; // any player may start a race; else only the MASTER
    bool owner = false;     // this player starts the races (RelayLink::begin)
    bool racing = false;    // a race's ring is formed
    std::vector<std::string> waiting; // in the room, not in the race: the next one
    int me = 0; // this player's line in names, 1-based; 0: in waiting
};
std::optional<Status> parse_status(const std::string &payload);

std::string percent_encode(const std::string &s);
std::string percent_decode(const std::string &s);

// The wire frames: type, epoch (big endian), length (big endian), payload.
constexpr uint8_t kData = 'D', kStatus = 'S', kPing = 'P', kError = 'E';
constexpr uint8_t kControl = 'C'; // to every other player of the room (rt::online::CourseSync)
constexpr uint8_t kBegin = 'G';   // the room's owner starts a race with everyone in the room
constexpr size_t kHeader = 7, kMaxPayload = 16 * 1024;
void append_frame(std::string &out, uint8_t type, uint32_t epoch, const uint8_t *payload, size_t n);

} // namespace app::online
