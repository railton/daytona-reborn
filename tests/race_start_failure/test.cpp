#include "common/race_start_failure.h"
#include "runtime/race_request.h"

#include <exception>
#include <iostream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

int g_failures = 0;

void expect(bool cond, const char *message) {
    if (!cond) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void expect_failure_journals_once(rt::RaceStatus status, const char *diagnostic) {
    std::vector<std::string> order;
    int calls = 0;
    try {
        tools::finalize_controlled_solo_failure(status, [&] {
            ++calls;
            order.push_back("end");
        });
        order.push_back("returned");
    } catch (const std::runtime_error &error) {
        order.push_back(error.what());
    } catch (const std::exception &error) {
        order.push_back(std::string("unexpected:") + error.what());
    }
    expect(calls == 1, "finalizer runs exactly once");
    expect(order.size() == 2 && order[0] == "end" && order[1] == diagnostic,
           "finalizer runs before the existing diagnostic");
}

void expect_quiet(rt::RaceStatus status) {
    int calls = 0;
    bool threw = false;
    try {
        tools::finalize_controlled_solo_failure(status, [&] { ++calls; });
    } catch (const std::exception &) {
        threw = true;
    }
    expect(calls == 0, "non-failure status must not finalize");
    expect(!threw, "non-failure status must not throw");
}

void expect_io_propagates(rt::RaceStatus status) {
    int calls = 0;
    bool saw_io = false;
    bool saw_diagnostic = false;
    try {
        tools::finalize_controlled_solo_failure(status, [&] {
            ++calls;
            throw std::ios_base::failure("journal write failed");
        });
    } catch (const std::ios_base::failure &error) {
        saw_io = std::string(error.what()).find("journal write failed") != std::string::npos;
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        saw_diagnostic = message == "PC solo start timed out" ||
                         message == "PC solo selected the wrong course";
    } catch (const std::exception &) {
        saw_io = false;
    }
    expect(calls == 1, "failed finalizer is not retried");
    expect(saw_io, "finalizer I/O failure propagates");
    expect(!saw_diagnostic, "I/O failure is not replaced with a successful diagnostic end");
}

int main() {
    expect_failure_journals_once(rt::RaceStatus::TimedOut, "PC solo start timed out");
    expect_failure_journals_once(rt::RaceStatus::WrongCourse, "PC solo selected the wrong course");
    expect_failure_journals_once(rt::RaceStatus::WrongMode, "PC solo selected the wrong mode");
    expect_failure_journals_once(rt::RaceStatus::WrongTransmission, "PC solo selected the wrong transmission");

    // Finished is attract re-entry, not victory.
    expect_quiet(rt::RaceStatus::Racing);
    expect_quiet(rt::RaceStatus::Starting);
    expect_quiet(rt::RaceStatus::Finished);
    expect_quiet(rt::RaceStatus::Idle);
    expect_quiet(rt::RaceStatus::Cancelled);

    expect_io_propagates(rt::RaceStatus::TimedOut);
    expect_io_propagates(rt::RaceStatus::WrongCourse);
    expect_io_propagates(rt::RaceStatus::WrongMode);
    expect_io_propagates(rt::RaceStatus::WrongTransmission);

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
