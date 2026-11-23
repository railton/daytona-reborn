// app/online and runtime/online_settings: the invite code, the relay
// protocol's text, and the cabinet settings written for an online race.
#include "app/online.h"
#include "runtime/online_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

namespace {
// A board with just what configure() touches; the factory settings at
// 0x239d60 are a synthetic stand-in (no ROM data in tests).
struct FakeBoard {
    struct Io { std::array<uint8_t, 128> eeprom{}; } io_;
    std::array<uint8_t, 16384> backup{};
    Io &io() { return io_; }
    std::array<uint8_t, 16384> &backup_ram() { return backup; }
    uint8_t read_byte(uint32_t address) const {
        const uint32_t i = address - 0x239d60;
        if (i < 8) return uint8_t("SEGA@\x82\x02\x00"[i]);
        return i == 0x0b ? 1 : 0; // factory: MASTER
    }
};
} // namespace

int main() {
    using namespace app::online;

    // invite: round trip, any case, with or without dashes and spaces
    const Invite in{"vps.exemplo.com.br:7777", "k7f3q9xz", "s3nh@ forte"};
    const std::string code = encode_invite(in);
    CHECK(code.rfind("DTN1-", 0) == 0);
    auto out = decode_invite(code);
    CHECK(out && out->server == in.server && out->room == in.room && out->password == in.password);
    std::string messy;
    for (char c : code)
        if (c != '-') messy += char(std::tolower(static_cast<unsigned char>(c)));
    out = decode_invite("  " + messy + " ");
    CHECK(out && out->room == in.room);
    CHECK(!decode_invite(""));
    CHECK(!decode_invite("DTN1-"));
    CHECK(!decode_invite("XXXX-ABCDE"));
    CHECK(!decode_invite(code.substr(0, code.size() - 7))); // cut short
    std::string typo = code;
    typo[8] = typo[8] == 'A' ? 'B' : 'A';
    CHECK(!decode_invite(typo)); // one wrong character
    CHECK(!decode_invite(encode_invite({"host:7777", "Sala Ruim!", ""})));

    // server addresses
    CHECK(normalize_server("vps.exemplo.com") == std::string("vps.exemplo.com:7777"));
    CHECK(normalize_server(" 192.168.0.10:9000 ") == std::string("192.168.0.10:9000"));
    CHECK(normalize_server("[2804:14c::1]:7777") == std::string("[2804:14c::1]:7777"));
    CHECK(normalize_server("2804:14c::1") == std::string("[2804:14c::1]:7777"));
    CHECK(!normalize_server(""));
    CHECK(!normalize_server("host:0"));
    CHECK(!normalize_server("host:70000"));
    CHECK(!normalize_server("host:abc"));

    // rooms and names
    const std::string room = random_room();
    CHECK(room.size() == 8 && valid_room(room) && room != random_room());
    CHECK(clean_name("  Ana\x01 Maria  ") == "Ana Maria");
    CHECK(clean_name("Jo\xC3\xA3o") == "Jo\xC3\xA3o");
    CHECK(clean_name("abcdefghijklmnopqrstuvwxyz").size() == kMaxName);

    // hello line (the Go server's ParseHello reads these)
    Hello h;
    h.op = Hello::Op::Create;
    h.room = "abc";
    h.players = 3;
    h.name = "Jo\xC3\xA3o S";
    h.password = "a b";
    CHECK(hello_line(h) == "DAYTONA-RELAY 1 op=create room=abc players=3 name=Jo%C3%A3o%20S pass=a%20b\n");
    h.op = Hello::Op::Join;
    h.name.clear();
    h.password.clear();
    CHECK(hello_line(h) == "DAYTONA-RELAY 1 op=join room=abc\n");

    // status (as the Go server's Status.Encode writes it); offsets the
    // server may not set are dropped
    auto st = parse_status("joined=2 size=3 pos=2 ready=0\neeprom=1a:01,20:03,0b:00,10:1a\nname=Railton\nname=Jo%C3%A3o\n");
    CHECK(st && st->joined == 2 && st->size == 3 && st->position == 2 && !st->ready);
    CHECK(st->eeprom.size() == 3 && st->eeprom[0].offset == 0x1a && st->eeprom[1].value == 3 && st->eeprom[2].offset == 0x10);
    CHECK(st->names.size() == 2 && st->names[1] == "Jo\xC3\xA3o");
    CHECK(!parse_status("garbage"));

    // frames
    std::string wire;
    const uint8_t payload[] = {9, 8};
    append_frame(wire, kData, 0x01020304, payload, 2);
    CHECK(wire == std::string("D\x01\x02\x03\x04\x00\x02\x09\x08", 9));

    // settings CRC: CRC-16/XMODEM of 0x0a..0x7f
    std::array<uint8_t, 128> zeros{};
    CHECK(rt::online::settings_crc(zeros) == 0);
    zeros[0x7f] = 1;
    CHECK(rt::online::settings_crc(zeros) == 0x1021);

    // configure: invalid settings start from the factory's; the place in the
    // room sets LINK ID and CAR NUMBER; only the race settings are taken;
    // the CRC is right; backup RAM holds both copies
    FakeBoard board;
    rt::online::configure(board, 3, {{0x1a, 0x00}, {0x20, 0x03}, {0x0b, 0x00}, {0x40, 0x55}});
    const auto &e = board.io_.eeprom;
    CHECK(e[0] == 'S' && e[3] == 'A');
    CHECK(e[0x0b] == 2 && e[0x0c] == 2); // SLAVE, car 3
    CHECK(e[0x1a] == 0 && e[0x20] == 3 && e[0x40] == 0);
    CHECK((e[8] | (e[9] << 8)) == rt::online::settings_crc(e));
    CHECK(std::equal(e.begin(), e.end(), board.backup.begin()) &&
          std::equal(e.begin(), e.end(), board.backup.begin() + 128));
    rt::online::configure(board, 1, {});
    CHECK(e[0x0b] == 1 && e[0x0c] == 0 && e[0x20] == 3); // MASTER car 1; earlier valid settings kept
    std::puts("online ok");
    return 0;
}
