#pragma once

#include <optional>
#include <string_view>

namespace rt {

// PC product contract. No cabinet registers, EEPROM, credits or input pulses.
enum class RaceCourse : unsigned { Beginner, Advanced, Expert };
enum class RaceTransmission : unsigned { Automatic, Manual };
enum class RaceMode : unsigned { Normal, TimeAttack };
constexpr bool valid_race_mode(RaceMode mode) {
    return unsigned(mode) <= unsigned(RaceMode::TimeAttack);
}
constexpr std::optional<RaceMode> parse_race_mode(std::string_view name) {
    if (name == "normal") return RaceMode::Normal;
    if (name == "time-attack") return RaceMode::TimeAttack;
    return std::nullopt;
}
struct StartRaceRequest {
    RaceCourse course = RaceCourse::Beginner;
    RaceTransmission transmission = RaceTransmission::Automatic;
    RaceMode mode = RaceMode::Normal;
    // The player picks on the cabinet's own selection screens, as at the
    // arcade (the PC home): the course, the gearbox, and Time Attack (Start
    // held while confirming the gearbox); course, transmission and mode are
    // then not used. Else the start answers them with this request's
    // (--solo, tests).
    bool player_chooses = false;
    // The cabinet's COUNTRY (0 USA: English, 1 JPN: Japanese, 2 EXPORT) and
    // DIFFICULTY (0 easy .. 3 hardest) for this start, written to its
    // settings with the rest; -1: as they are (Revision A's factory: JPN).
    int country = -1, difficulty = -1;
};
constexpr bool valid_course(RaceCourse course) {
    return unsigned(course) <= unsigned(RaceCourse::Expert);
}
constexpr bool valid_transmission(RaceTransmission transmission) {
    return unsigned(transmission) <= unsigned(RaceTransmission::Manual);
}
constexpr std::optional<RaceTransmission> parse_race_transmission(std::string_view name) {
    if (name == "automatic") return RaceTransmission::Automatic;
    if (name == "manual") return RaceTransmission::Manual;
    return std::nullopt;
}

enum class RaceStartResult {
    Accepted, AlreadyPending, ConflictingRequest, InvalidCourse,
    RaceAlreadyActive, RequiresFreshGame, UnsupportedRevision, NetworkConfigured,
    InvalidTransmission, InvalidMode
};
enum class RaceStatus { Idle, Starting, Racing, Finished, WrongCourse, TimedOut, Cancelled, WrongMode, WrongTransmission };

} // namespace rt
