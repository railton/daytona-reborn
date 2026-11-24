#pragma once

#include "runtime/pc_solo.h"
#include "runtime/race_request.h"

#include <optional>

namespace rt {

// The compatibility implementation is private to the runtime. Request before
// the first tick, after loading saved settings. A fresh board is still booting,
// not in attract. Repeated requests cannot rewrite settings or reset pulses.
class RaceSession {
public:
    RaceSession() = default;
    RaceSession(const RaceSession &) = delete;
    RaceSession &operator=(const RaceSession &) = delete;
    template <class Board>
    RaceStartResult request(StartRaceRequest request, Board &board, bool fresh) {
        if (!valid_course(request.course)) return RaceStartResult::InvalidCourse;
        if (!valid_transmission(request.transmission)) return RaceStartResult::InvalidTransmission;
        if (!valid_race_mode(request.mode)) return RaceStartResult::InvalidMode;
        if (status_ == RaceStatus::Starting)
            return request.course == course_ && request.transmission == transmission_ && request.mode == mode_
                ? RaceStartResult::AlreadyPending : RaceStartResult::ConflictingRequest;
        if (status_ == RaceStatus::Racing || PcSolo::in_race(board))
            return RaceStartResult::RaceAlreadyActive;
        if (status_ != RaceStatus::Idle || !fresh) return RaceStartResult::RequiresFreshGame;
        PcSolo::configure(board, request.country, request.difficulty);
        course_ = request.course;
        transmission_ = request.transmission;
        mode_ = request.mode;
        player_chooses_ = request.player_chooses;
        adapter_.emplace(course_, transmission_, mode_, player_chooses_);
        status_ = RaceStatus::Starting;
        return RaceStartResult::Accepted;
    }

    template <class Board>
    Inputs input(Board &board, const Inputs &player) {
        if (status_ == RaceStatus::Starting) {
            if (player_chooses_ && PcSolo::selecting(board)) return player; // the cabinet's own screens: the player's, as at the arcade
            return adapter_->input(board);
        }
        if (status_ == RaceStatus::Cancelled || status_ == RaceStatus::WrongCourse ||
            status_ == RaceStatus::WrongMode || status_ == RaceStatus::WrongTransmission ||
            status_ == RaceStatus::TimedOut)
            return {}; // a terminal startup cannot keep generating input
        return player;
    }

    // Called exactly once after the simulation tick; the application never
    // polls guest RAM or clocks this state machine at presentation frequency.
    template <class Board>
    void observe(Board &board) {
        if (status_ == RaceStatus::Starting) {
            switch (adapter_->status(board)) {
            case PcSolo::Status::Starting: return;
            case PcSolo::Status::Racing: status_ = RaceStatus::Racing; break;
            case PcSolo::Status::WrongCourse: status_ = RaceStatus::WrongCourse; break;
            case PcSolo::Status::WrongMode: status_ = RaceStatus::WrongMode; break;
            case PcSolo::Status::WrongTransmission: status_ = RaceStatus::WrongTransmission; break;
            case PcSolo::Status::TimedOut: status_ = RaceStatus::TimedOut; break;
            }
            adapter_.reset();
        } else if (status_ == RaceStatus::Racing && PcSolo::in_attract(board)) {
            status_ = RaceStatus::Finished;
        }
    }

    // Cancels only startup, at the caller's frame boundary. It is not a rollback
    // of the guest's partially completed boot; create a fresh GameLoop to retry.
    bool cancel() {
        if (status_ != RaceStatus::Starting) return false;
        adapter_.reset();
        status_ = RaceStatus::Cancelled;
        return true;
    }
    RaceStatus status() const { return status_; }

private:
    RaceStatus status_ = RaceStatus::Idle;
    RaceCourse course_ = RaceCourse::Beginner;
    RaceTransmission transmission_ = RaceTransmission::Automatic;
    RaceMode mode_ = RaceMode::Normal;
    bool player_chooses_ = false;
    std::optional<PcSolo> adapter_;
};

} // namespace rt
