#pragma once

#include "runtime/race_request.h"

#include <stdexcept>
#include <utility>

namespace tools {

// Controlled solo-start failures journal their terminal event before the
// existing diagnostic is raised. Finished is attract re-entry, not victory
// and not one of those failures. If the finalizer throws, that error
// propagates and the diagnostic is not substituted.
template <class Finalize>
void finalize_controlled_solo_failure(rt::RaceStatus status, Finalize &&finalize) {
    const char *diagnostic = nullptr;
    if (status == rt::RaceStatus::TimedOut)
        diagnostic = "PC solo start timed out";
    else if (status == rt::RaceStatus::WrongCourse)
        diagnostic = "PC solo selected the wrong course";
    else if (status == rt::RaceStatus::WrongMode)
        diagnostic = "PC solo selected the wrong mode";
    else if (status == rt::RaceStatus::WrongTransmission)
        diagnostic = "PC solo selected the wrong transmission";
    else
        return;

    std::forward<Finalize>(finalize)();
    throw std::runtime_error(diagnostic);
}

} // namespace tools
