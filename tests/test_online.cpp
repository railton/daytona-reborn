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
    int mode = 3, step = 0, course = 0, rank = 0; // the guest's mode dispatch, selection step, course cursor, place
    uint8_t read_byte(uint32_t address) const {
        if (address == rt::online::AutoEntry::mode_address) return uint8_t(mode);
        if (address == rt::online::CourseSync::step_address) return uint8_t(step);
        if (address == rt::online::CourseSync::course_address) return uint8_t(course);
        if (address == rt::online::Standings::rank_address) return uint8_t(rank);
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
    h.name = "Jo\xC3\xA3o S";
    h.password = "a b";
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=create room=abc name=Jo%C3%A3o%20S pass=a%20b v=" +
                               std::to_string(kOnlineVersion) + "\n");
    h.op = Hello::Op::Join;
    h.name.clear();
    h.password.clear();
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=join room=abc v=" + std::to_string(kOnlineVersion) + "\n");
    // settings go with a create only; info has no room
    h.settings = "difficulty:hard,start:anyone";
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=join room=abc v=" + std::to_string(kOnlineVersion) + "\n");
    h.op = Hello::Op::Create;
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=create room=abc v=" + std::to_string(kOnlineVersion) +
                               " set=difficulty%3Ahard%2Cstart%3Aanyone\n");
    h.op = Hello::Op::Info;
    h.password = "pw";
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=info pass=pw v=" + std::to_string(kOnlineVersion) + "\n");
    h.udp = true; // UDP is for a room's data, not for info
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=info pass=pw v=" + std::to_string(kOnlineVersion) + "\n");
    // back after a drop: the key the server gave, and UDP again
    h.op = Hello::Op::Rejoin;
    h.password.clear();
    h.settings.clear();
    h.key = "0a1b";
    CHECK(hello_line(h) == "DAYTONA-RELAY 5 op=rejoin room=abc v=" + std::to_string(kOnlineVersion) + " key=0a1b udp=1\n");
    // the server's answer (Race.Info)
    const auto info = parse_info("cabinet=twin\ndifficulty=hard\nbad\n=x\n");
    CHECK(info.size() == 2 && info[0].first == "cabinet" && info[1].second == "hard");

    // status (as the Go server's Status.Encode writes it); offsets the
    // server may not set are dropped
    auto st = parse_status("joined=2 size=3 pos=2 ready=0\neeprom=1a:01,20:03,0b:00,10:1a\nname=Railton\nname=Jo%C3%A3o\n");
    CHECK(st && st->joined == 2 && st->size == 3 && st->position == 2 && !st->ready);
    CHECK(st->eeprom.size() == 3 && st->eeprom[0].offset == 0x1a && st->eeprom[1].value == 3 && st->eeprom[2].offset == 0x10);
    CHECK(st->names.size() == 2 && st->names[1] == "Jo\xC3\xA3o");
    CHECK(!st->any_start);
    st = parse_status("joined=2 size=2 pos=1 ready=1\neeprom=\nname=A\nname=B\nstart=anyone\n");
    CHECK(st && !st->owner && !st->racing && st->waiting.empty() && st->me == 0);
    // a race of 2 with a third waiting for the next one (as the Go server's Status.Encode writes it)
    auto race = parse_status("joined=3 size=2 pos=1 ready=1\neeprom=\nname=A\nname=B\nstart=master\n"
                             "owner=1\nracing=1\nme=1\nwaiting=Ana%20Maria\n");
    CHECK(race && race->owner && race->racing && race->me == 1 && race->size == 2 && race->joined == 3);
    CHECK(race->waiting.size() == 1 && race->waiting[0] == "Ana Maria");
    // latencies (ms, round trip), as names and waiting; empty: not measured yet
    auto lat = parse_status("joined=3 size=0 pos=0 ready=0\neeprom=\nname=A\nname=B\nwaiting=C\nlat=18,\nwlat=132\n");
    CHECK(lat && lat->latency.size() == 2 && lat->latency[0] == 18 && lat->latency[1] == -1);
    CHECK(lat->waiting_latency.size() == 1 && lat->waiting_latency[0] == 132);
    CHECK(st && st->ready && st->any_start && st->eeprom.empty());
    // a star-mode relay: this player's key, and places kept through a drop
    auto keyed = parse_status("joined=2 size=2 pos=2 ready=1\neeprom=\nname=A\nname=B\nkey=0a1b\nresume=1\n");
    CHECK(keyed && keyed->key == "0a1b" && keyed->resume);
    CHECK(!parse_status("joined=2 size=2 pos=2 ready=1\neeprom=\nname=A\nname=B\n")->resume);
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

    // AutoEntry: Start only while another cabinet's race waits for this
    // one's entry, in short edges, nothing otherwise
    struct In { uint8_t in0 = 0xff, steer = 0x80, accel = 0x20; };
    rt::online::AutoEntry entry;
    int presses = 0;
    for (int f = 0; f < 200; ++f) {
        In in;
        entry.apply(board, in);
        CHECK(in.in0 == 0xff); // attract: untouched
    }
    board.mode = rt::online::AutoEntry::waiting_for_entry;
    bool held = false;
    for (int f = 0; f < 100; ++f) {
        In in;
        entry.apply(board, in);
        const bool start = !(in.in0 & 0x10);
        CHECK((in.in0 | 0x10) == 0xff); // Start only
        if (start && !held) ++presses;
        held = start;
    }
    CHECK(presses >= 3); // released between presses: edges, not a held button
    board.mode = 17;
    In after;
    entry.apply(board, after);
    CHECK(after.in0 == 0xff);

    // StartGate: a SLAVE's Start is released in attract, kept from the entry
    // countdown on; the MASTER's always kept
    board.mode = 3;
    In gate;
    gate.in0 = 0xef; // Start held
    rt::online::StartGate::apply(board, gate, false);
    CHECK(gate.in0 == 0xff);
    gate.in0 = 0xef;
    rt::online::StartGate::apply(board, gate, true);
    CHECK(gate.in0 == 0xef);
    board.mode = 16;
    rt::online::StartGate::apply(board, gate, false);
    CHECK(gate.in0 == 0xef);

    // CourseSync: the MASTER tells its cursor as it moves and its course on
    // confirmation; a SLAVE steers to it and confirms only after the MASTER
    rt::online::CourseSync master_side, slave_side;
    FakeBoard master_board, slave_board;
    master_board.mode = slave_board.mode = 18;
    master_board.step = slave_board.step = 1;
    master_board.course = 0;
    CHECK(master_side.master(master_board) == "cursor=0");
    CHECK(master_side.master(master_board).empty()); // nothing new
    master_board.course = 1; // long: Expert
    const std::string moved = master_side.master(master_board);
    CHECK(moved == "cursor=1");
    slave_side.receive(moved);
    In s1;
    for (int f = 0; f < 60; ++f) {
        s1 = {};
        s1.accel = 0xe0; // the SLAVE's own pedal: ignored here
        slave_side.slave(slave_board, s1);
        CHECK(s1.steer == 0xe0 && s1.accel == 0x20); // steering to Expert, not confirming
    }
    master_board.step = 2; // the MASTER confirms
    const std::string chosen = master_side.master(master_board);
    CHECK(chosen == "course=1");
    slave_side.receive(chosen);
    bool confirmed = false;
    for (int f = 0; f < 60; ++f) {
        In in;
        slave_side.slave(slave_board, in);
        CHECK(in.steer == 0xe0);
        confirmed |= in.accel == 0xe0;
    }
    CHECK(confirmed && slave_side.confirmed());
    slave_board.mode = 22; // racing: the next selection starts afresh
    In racing;
    racing.steer = 0x30;
    slave_side.slave(slave_board, racing);
    CHECK(racing.steer == 0x30 && slave_side.cursor() == -1 && !slave_side.confirmed());
    slave_side.receive("course=7"); // out of range: ignored
    CHECK(slave_side.cursor() == -1);

    // Standings: this cabinet's place told on a change (and every 2 s), the
    // others' kept, everything cleared outside a race
    rt::online::Standings standings;
    FakeBoard racer;
    racer.mode = 22;
    racer.rank = 2;
    CHECK(standings.own(racer, 3) == "pos=3:2");
    CHECK(standings.own(racer, 3).empty());
    racer.rank = 0;
    CHECK(standings.own(racer, 3) == "pos=3:0");
    CHECK(standings.receive("pos=1:1") && standings.rank(1) == 1 && standings.rank(3) == 0);
    CHECK(standings.receive("pos=9:1") && standings.rank(9) == -1); // ours, out of range: ignored
    CHECK(!standings.receive("course=1"));
    int repeats = 0;
    for (int f = 0; f < 240; ++f) repeats += !standings.own(racer, 3).empty();
    CHECK(repeats == 2); // every 115 frames while unchanged
    racer.mode = 3;
    CHECK(standings.own(racer, 3).empty() && standings.rank(1) == -1 && standings.rank(3) == -1);
    std::puts("online ok");
    return 0;
}
