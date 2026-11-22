// Synthetic host-side checks. No guest image and no RAM writes.
#include "common/probe_evidence.h"

#include <iostream>

namespace {

int fail(const char *message) {
    std::cerr << message << '\n';
    return 1;
}

int test_entry_without_window() {
    tools::SelectionEvidence evidence;
    evidence.note(22, 0, 0x80, 0xe0);
    evidence.note(3, 0, 0xe0, 0xe0);
    if (evidence.samples != 0) return fail("window-free observations counted");
    evidence.observe_entry(18, 22, 1);
    if (evidence.note(18, 7, 0xe0, 0xe0)) return fail("late selection counted after race entry");
    if (evidence.success()) return fail("race entry without a gearbox window succeeded");
    if (evidence.success()) return fail("empty evidence succeeded");
    return 0;
}

int test_actual_samples() {
    tools::SelectionEvidence evidence;
    if (!evidence.note(18, 7, 0xe0, 0xe0)) return fail("matching sample was ignored");
    if (!evidence.note(18, 7, 0x80, 0x80)) return fail("center steer in the window was ignored");
    if (evidence.samples != 2) return fail("actual selection samples were not both counted");
    if (evidence.success()) return fail("samples without entry passed");
    if (evidence.observe_entry(22, 22, 40)) return fail("initial race mode counted as transition");
    evidence.observe_entry(18, 22, 42);
    if (!evidence.success()) return fail("entry plus samples did not pass");
    return 0;
}

int test_rejects() {
    tools::SelectionEvidence evidence;
    if (evidence.note(17, 7, 0xe0, 0xe0)) return fail("wrong mode counted");
    if (evidence.note(18, 6, 0xe0, 0xe0)) return fail("wrong step counted");
    if (evidence.note(18, 7, 0x80, 0xe0)) return fail("mismatched steer counted");
    if (evidence.note(18, 7, 0xe0, 0x80)) return fail("requested steer was not the applied byte");
    if (evidence.samples != 0) return fail("rejected observations changed the count");
    if (evidence.success()) return fail("rejected observations still passed");
    return 0;
}

int test_ports_and_clock() {
    tools::require_selection_ports(0xff, 0x8f, 0xff);
    tools::require_selection_ports(0xef, 0x8f, 0xff);
    tools::require_frame_step(0, 1);
    tools::require_frame_step(1399, 1400);
    bool ports = false;
    try {
        tools::require_selection_ports(0xdf, 0x8f, 0xff);
    } catch (const std::runtime_error &) {
        ports = true;
    }
    if (!ports) return fail("non-Start IN0 was accepted");
    bool in1 = false;
    try {
        tools::require_selection_ports(0xff, 0x8e, 0xff);
    } catch (const std::runtime_error &) {
        in1 = true;
    }
    if (!in1) return fail("non-default IN1 was accepted");
    bool in2 = false;
    try {
        tools::require_selection_ports(0xff, 0x8f, 0xfe);
    } catch (const std::runtime_error &) {
        in2 = true;
    }
    if (!in2) return fail("non-default IN2 was accepted");
    bool clock = false;
    try {
        tools::require_frame_step(4, 4);
    } catch (const std::runtime_error &) {
        clock = true;
    }
    if (!clock) return fail("equal tick and frame were accepted");
    return 0;
}

} // namespace

int main() {
    if (int rc = test_entry_without_window()) return rc;
    if (int rc = test_actual_samples()) return rc;
    if (int rc = test_rejects()) return rc;
    if (int rc = test_ports_and_clock()) return rc;
    return 0;
}
