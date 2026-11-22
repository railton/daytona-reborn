#include "runtime/camera_lineage.h"

#include <cstdio>
#include <cstdlib>

namespace {

using rt::BufferWriteProvenance;
using rt::CameraLineage;
using rt::WriteOrigin;

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

struct Board {
    CameraLineage camera{8};
    BufferWriteProvenance memory{8};

    void focus(std::size_t word, uint8_t mode) {
        camera.read_mode(mode);
        for (std::size_t index = word; index < word + 2; ++index) {
            require(memory.record(index, 0xffffffffu, WriteOrigin::GeometryPort),
                    "fixture geometry port write");
            camera.record_focus_word(index, memory.at(index).writes,
                                     value(index), static_cast<uint32_t>(16 + index), 3);
        }
    }

    void object(std::size_t word) {
        camera.observe_focus(word, value(word), value(word + 1), &memory);
        camera.observe_world_object();
    }

    CameraLineage::DrawEvidence draw(std::size_t word) {
        camera.begin_scene();
        object(word);
        return camera.finish_scene();
    }

private:
    static uint32_t value(std::size_t word) { return static_cast<uint32_t>(210 + word); }
};

void acquisition_and_board_scope() {
    Board first, second;
    first.focus(0, 7);
    const auto acquired = first.draw(0);
    require(acquired.revision == 1 && acquired.tagged == 1 && acquired.hold && !acquired.prior,
            "first acquisition protects the changed frame");
    require(first.draw(0).hold, "first acquisition protects the next edge");
    require(!first.draw(0).hold, "stable camera resumes interpolation");

    require(second.memory.record(0, 0xffffffffu, WriteOrigin::GeometryPort) &&
            second.memory.record(1, 0xffffffffu, WriteOrigin::GeometryPort),
            "fixture second board writes");
    const auto unknown = second.draw(0);
    require(!unknown.revision && !unknown.tagged && unknown.untagged == 1,
            "matching write counters cannot share evidence across boards");
    second.focus(4, 4);
    require(second.draw(4).revision == 1, "each board has its own revision counter");
}

void mixed_revisions_then_clean_scene() {
    Board board;
    board.focus(0, 7);
    board.draw(0);
    board.focus(2, 11);

    board.camera.begin_scene();
    board.object(0);
    board.object(2);
    const auto mixed = board.camera.finish_scene();
    require(mixed.conflict && mixed.tagged == 2 && mixed.hold, "mixed revisions must hold");

    const auto clean = board.draw(2);
    require(!clean.conflict && clean.revision == 2 && !clean.prior && clean.hold,
            "first clean revision after conflict protects the changed frame");
    require(board.draw(2).hold, "first clean revision after conflict protects the next edge");
    require(!board.draw(2).hold, "clean revision resumes interpolation");
}

void partial_overwrite_and_reacquisition() {
    Board board;
    board.focus(0, 7);
    board.draw(0);
    board.draw(0);
    board.draw(0);

    require(board.memory.record(0, 0xffu, WriteOrigin::I960), "fixture partial overwrite");
    const auto stale = board.draw(0);
    require(!stale.revision && !stale.tagged && stale.untagged == 1,
            "partial overwrite invalidates focus even if values still match");
    require(!board.draw(7).revision, "focus at the last buffer word cannot form a pair");

    board.focus(0, 7);
    const auto reacquired = board.draw(0);
    require(reacquired.revision == 1 && !reacquired.prior && reacquired.hold,
            "reacquisition after an unknown interval protects the changed frame");
    require(board.draw(0).hold, "reacquisition protects the next edge");
    require(!board.draw(0).hold, "reacquired revision resumes interpolation");
}

void discard_draw_history() {
    Board board;
    board.focus(0, 7);
    board.draw(0);
    board.draw(0);
    board.draw(0);

    board.camera.discard_draw_history();
    const auto restarted = board.draw(0);
    require(restarted.revision == 1 && !restarted.prior && restarted.hold,
            "discard retains live focus evidence and resets drawn history");
    require(board.draw(0).hold, "discard protects the next edge");
    require(!board.draw(0).hold, "draw history resumes after discard");
}

} // namespace

int main() {
    acquisition_and_board_scope();
    mixed_revisions_then_clean_scene();
    partial_overwrite_and_reacquisition();
    discard_draw_history();
}
