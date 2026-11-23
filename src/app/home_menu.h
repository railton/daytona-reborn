#pragma once

#include "runtime/race_request.h"

struct ImFont;

namespace app {

class HomeMenu {
public:
    enum Action { Stay, Play, Resume, Settings, Quit, Cancel, Online };
    void init();
    void select(rt::StartRaceRequest request) {
        course_ = request.course;
        transmission_ = request.transmission;
        mode_ = request.mode;
    }
    Action draw(bool has_race, bool loading, const char *message, float opacity);
    rt::RaceCourse course() const { return course_; }
    rt::StartRaceRequest request() const { return {course_, transmission_, mode_}; }

private:
    enum class Screen { Ready, Loading, Paused };
    Screen screen_ = Screen::Ready;
    rt::RaceCourse course_ = rt::RaceCourse::Beginner;
    rt::RaceTransmission transmission_ = rt::RaceTransmission::Automatic;
    rt::RaceMode mode_ = rt::RaceMode::Normal;
    ImFont *display_ = nullptr, *body_ = nullptr;
};

} // namespace app
