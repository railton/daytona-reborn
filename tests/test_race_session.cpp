#include "runtime/race_session.h"

#include <array>
#include <cstdio>
#include <optional>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

namespace {
struct Board {
    struct Io { std::array<uint8_t, 128> eeprom{}; } settings;
    std::vector<uint8_t> backup = std::vector<uint8_t>(256);
    unsigned reads = 0, config_accesses = 0;
    uint32_t race_mode = 0;
    uint8_t mode = 0, step = 0, course = 0, transmission = 0;
    Io &io() { ++config_accesses; return settings; }
    auto &backup_ram() { return backup; }
    uint32_t read_dword(uint32_t address) {
        ++reads;
        return address == 0x501a80 ? race_mode : 0;
    }
    uint8_t read_byte(uint32_t address) {
        ++reads;
        if (address == 0x5010a0) return mode;
        if (address == 0x5010f8) return step;
        if (address == 0x501460) return course;
        if (address == 0x501464) return transmission;
        return 0; // synthetic factory settings, no ROM bytes
    }
};
bool same(const rt::Inputs &a, const rt::Inputs &b) {
    return a.steer == b.steer && a.accel == b.accel && a.brake == b.brake &&
           a.in0 == b.in0 && a.in1 == b.in1 && a.in2 == b.in2;
}
}

int main() {
    using C = rt::RaceCourse;
    using R = rt::RaceStartResult;
    using S = rt::RaceStatus;
    using T = rt::RaceTransmission;
    using M = rt::RaceMode;

    CHECK(rt::parse_race_transmission("automatic") == T::Automatic);
    CHECK(rt::parse_race_transmission("manual") == T::Manual);
    CHECK(!rt::parse_race_transmission(""));
    CHECK(!rt::parse_race_transmission("Manual"));
    CHECK(!rt::parse_race_transmission("manual "));
    CHECK(rt::parse_race_mode("normal") == M::Normal);
    CHECK(rt::parse_race_mode("time-attack") == M::TimeAttack);
    CHECK(!rt::parse_race_mode("Time Attack"));

    // Validate even an explicitly cast invalid enum before touching the board.
    Board invalid;
    rt::RaceSession rejected;
    CHECK(rejected.request({C(3)}, invalid, true) == R::InvalidCourse);
    CHECK(rejected.request({C(~0u)}, invalid, true) == R::InvalidCourse);
    CHECK(invalid.reads == 0 && invalid.config_accesses == 0);
    CHECK(rejected.status() == S::Idle);
    CHECK(rejected.request({C::Beginner, rt::RaceTransmission(2)}, invalid, true) == R::InvalidTransmission);
    CHECK(invalid.reads == 0 && invalid.config_accesses == 0);

    CHECK(rejected.request({C::Beginner, T::Automatic, M(2)}, invalid, true) == R::InvalidMode);
    CHECK(invalid.reads == 0 && invalid.config_accesses == 0);

    for (uint32_t actual : {0u, 1u, 257u}) {
        Board attack_board;
        rt::RaceSession attack;
        const rt::StartRaceRequest request{C::Beginner, T::Manual, M::TimeAttack};
        CHECK(attack.request(request, attack_board, true) == R::Accepted);
        CHECK(attack.request(request, attack_board, false) == R::AlreadyPending);
        CHECK(attack.request({C::Beginner, T::Manual}, attack_board, false) == R::ConflictingRequest);
        attack_board.mode = 18;
        attack_board.step = 1;
        CHECK((attack.input(attack_board, {}).in0 & 0x10) != 0);
        attack_board.step = 7;
        for (unsigned tick = 1; tick <= 35; ++tick) {
            const auto input = attack.input(attack_board, {});
            CHECK(input.steer == 0xe0 && (input.in0 & 0x10) == 0);
            CHECK(input.accel == (tick >= 30 && tick <= 34 ? 0xe0 : 0x20));
        }
        attack_board.mode = 22;
        attack_board.race_mode = actual;
        attack_board.transmission = 1;
        attack.observe(attack_board);
        CHECK(attack.status() == (actual == 1 ? S::Racing : S::WrongMode));
        rt::Inputs player;
        player.accel = 0xa0;
        CHECK(same(attack.input(attack_board, player), actual == 1 ? player : rt::Inputs{}));
    }

    for (const auto requested : {T::Automatic, T::Manual}) {
        for (uint8_t actual : {0, 1, 2}) {
            Board observed;
            rt::RaceSession session;
            CHECK(session.request({C::Beginner, requested}, observed, true) == R::Accepted);
            observed.mode = 22;
            observed.transmission = actual;
            session.observe(observed);
            const bool matched = actual == unsigned(requested);
            CHECK(session.status() == (matched ? S::Racing : S::WrongTransmission));
            rt::Inputs player;
            player.accel = 0xa0;
            CHECK(same(session.input(observed, player), matched ? player : rt::Inputs{}));
        }
    }

    // Gearbox changes are conflicting requests, not idempotent retries. They
    // cannot rewrite settings or change a request that is already in flight.
    Board manual_board;
    rt::RaceSession manual;
    CHECK(manual.request({C::Beginner, rt::RaceTransmission::Manual}, manual_board, true) == R::Accepted);
    CHECK(manual.request({C::Beginner, rt::RaceTransmission::Manual}, manual_board, false) == R::AlreadyPending);
    CHECK(manual.request({C::Beginner}, manual_board, false) == R::ConflictingRequest);
    CHECK(manual_board.config_accesses == 1);
    manual_board.mode = 18; manual_board.step = 1;
    CHECK(manual.input(manual_board, {}).steer == 0x80); // course selection unaffected
    manual_board.step = 7;
    for (unsigned tick = 1; tick <= 35; ++tick) {
        const auto input = manual.input(manual_board, {});
        CHECK(input.steer == 0xe0);
        CHECK(input.accel == (tick >= 30 && tick <= 34 ? 0xe0 : 0x20));
    }
    manual_board.mode = 22; manual_board.course = 0; manual_board.transmission = 1;
    manual.observe(manual_board);
    rt::Inputs driver; driver.steer = 0x43; driver.accel = 0x55;
    CHECK(same(manual.input(manual_board, driver), driver));

    // Original frontend used configure() before the first boot tick, not attract.
    // Compare the adapter signals at every tick, including duplicate/conflicting
    // native requests. Three courses exercise both native-to-guest table orders.
    for (unsigned course = 0; course != 3; ++course) {
        Board board, reference;
        rt::RaceSession session;
        std::optional<rt::PcSolo> original(std::in_place, C(course));
        rt::PcSolo::configure(reference);
        CHECK(session.request({C(course)}, board, true) == R::Accepted);
        CHECK(board.settings.eeprom == reference.settings.eeprom);
        CHECK(board.backup == reference.backup);
        CHECK(board.config_accesses == 1);
        for (unsigned tick = 0; tick < 700; ++tick) {
            board.mode = reference.mode = tick < 100 ? 0 : tick < 300 ? 3 : tick < 600 ? 18 : 22;
            board.step = reference.step = tick < 500 ? 1 : 7;
            constexpr uint8_t table[] = {0, 2, 1};
            board.course = reference.course = table[course];
            if (tick == 15 || tick == 129 || tick == 329) {
                const auto settings = board.settings.eeprom;
                const auto backup = board.backup;
                CHECK(session.request({C(course)}, board, false) == R::AlreadyPending);
                CHECK(session.request({C((course + 1) % 3)}, board, false) == R::ConflictingRequest);
                CHECK(board.settings.eeprom == settings && board.backup == backup);
                CHECK(board.config_accesses == 1);
            }
            rt::Inputs player;
            player.accel = 0xbb;
            const auto expected = original ? original->input(reference) : player;
            const auto actual = session.input(board, player);
            CHECK(same(actual, expected));
            session.observe(board);
            if (original && original->status(reference) == rt::PcSolo::Status::Racing) original.reset();
        }
        CHECK(session.status() == S::Racing);
        CHECK(session.request({C(course)}, board, false) == R::RaceAlreadyActive);
        CHECK(!session.cancel());
        board.mode = 3; session.observe(board);
        CHECK(session.status() == S::Finished);
        CHECK(session.request({C(course)}, board, true) == R::RequiresFreshGame);
    }

    Board active; active.mode = 22;
    rt::RaceSession running;
    CHECK(running.request({C::Beginner}, active, true) == R::RaceAlreadyActive);
    CHECK(active.config_accesses == 0);
    active.mode = 3;
    CHECK(running.request({C::Beginner}, active, false) == R::RequiresFreshGame);
    CHECK(active.config_accesses == 0);

    // Timeout is latched after the same 1500 input ticks as the old adapter.
    Board stuck; stuck.mode = 3;
    rt::RaceSession timeout;
    CHECK(timeout.request({C::Beginner}, stuck, true) == R::Accepted);
    for (unsigned tick = 1; tick <= 1500; ++tick) {
        const auto input = timeout.input(stuck, {});
        if (tick == 29 || tick == 35) CHECK((input.in0 & 0x10) != 0);
        if (tick == 30 || tick == 34) CHECK((input.in0 & 0x10) == 0);
        timeout.observe(stuck);
        CHECK(timeout.status() == (tick < 1500 ? S::Starting : S::TimedOut));
    }
    CHECK(timeout.request({C::Beginner}, stuck, true) == R::RequiresFreshGame);
    CHECK(same(timeout.input(stuck, {}), rt::Inputs{}));

    Board cancel_board; cancel_board.mode = 3;
    rt::RaceSession cancelled;
    CHECK(cancelled.request({C::Beginner}, cancel_board, true) == R::Accepted);
    for (unsigned i = 0; i != 30; ++i) { cancelled.input(cancel_board, {}); cancelled.observe(cancel_board); }
    const auto settings = cancel_board.settings.eeprom;
    const auto backup = cancel_board.backup;
    CHECK(cancelled.cancel());
    CHECK(!cancelled.cancel());
    CHECK(cancelled.status() == S::Cancelled);
    CHECK(same(cancelled.input(cancel_board, {}), rt::Inputs{}));
    CHECK(cancelled.request({C::Expert}, cancel_board, true) == R::RequiresFreshGame);
    CHECK(cancel_board.settings.eeprom == settings && cancel_board.backup == backup);

    Board wrong; rt::RaceSession mismatch;
    CHECK(mismatch.request({C::Advanced}, wrong, true) == R::Accepted);
    wrong.mode = 22; wrong.course = 1;
    mismatch.input(wrong, {}); mismatch.observe(wrong);
    CHECK(mismatch.status() == S::WrongCourse);
    std::puts("race session: 2100 adapter-equivalent ticks, request guards, timeout and cancellation passed");
    // The PC home: the player picks on the cabinet's own screens, as at the
    // arcade; the start drives only up to them, then accepts the race whatever
    // the course, the gearbox and the mode.
    {
        Board board;
        rt::RaceSession session;
        rt::StartRaceRequest request{C::Beginner, T::Automatic, M::Normal};
        request.player_chooses = true;
        CHECK(session.request(request, board, true) == R::Accepted);
        const rt::Inputs player{0xe0, 0xe0, 0x20, 0xef, 0xff, 0xff}; // steering right, pedal down, Start held
        board.mode = 3; // attract: the start presses Start itself, the player's input is not used
        CHECK(!same(session.input(board, player), player));
        board.mode = 18; // the selection: course (step 1), gearbox (step 7) and between: the player's
        for (const uint8_t step : {1, 4, 7}) {
            board.step = step;
            CHECK(same(session.input(board, player), player));
        }
        board.mode = 22; // racing with the other course, the manual gearbox and Time Attack: accepted
        board.course = 1;
        board.transmission = 1;
        board.race_mode = 1;
        session.observe(board);
        CHECK(session.status() == S::Racing);
    }
    {   // the PC home's country and difficulty, written with the start's other settings
        Board board;
        rt::RaceSession session;
        rt::StartRaceRequest request;
        request.country = 0;    // USA: English
        request.difficulty = 2; // hard
        CHECK(session.request(request, board, true) == R::Accepted);
        CHECK(board.settings.eeprom[0x1b] == 0 && board.settings.eeprom[0x20] == 2 && board.settings.eeprom[0x0b] == 0);
        CHECK(board.backup[0x1b] == 0 && board.backup[0x20] == 2); // and its copies in the backup RAM
    }

}
