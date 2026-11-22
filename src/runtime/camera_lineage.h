#pragma once

#include "runtime/write_provenance.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace rt {

class CameraLineage {
public:
    struct DrawEvidence {
        uint64_t prior{}, revision{};
        unsigned tagged{}, untagged{};
        bool conflict{}, hold{};
    };

    explicit CameraLineage(std::size_t words) : words_(words) {}

    void read_mode(uint8_t value) {
        if (mode_ != value) {
            mode_ = value;
            ++revision_;
        }
    }

    void record_focus_word(std::size_t word, uint64_t version, uint32_t value,
                           uint32_t source, uint32_t parent) {
        if (revision_ && word < words_.size())
            words_[word] = {version, revision_, value, source, parent};
    }

    void begin_scene() {
        prior_ = scene_ && !conflict_ ? scene_ : 0;
        prior_transition_ = transition_;
        active_ = scene_ = 0;
        tagged_ = untagged_ = 0;
        conflict_ = transition_ = false;
    }

    void observe_focus(std::size_t word, uint32_t x, uint32_t y,
                       const BufferWriteProvenance *live) {
        active_ = 0;
        if (!live || word >= words_.size() || word >= live->size() ||
            words_.size() - word < 2 || live->size() - word < 2)
            return;
        const auto &a = words_[word], &b = words_[word + 1];
        const auto &lx = live->at(word), &ly = live->at(word + 1);
        constexpr auto full = std::numeric_limits<uint32_t>::max();
        if (a.revision && a.revision == b.revision &&
            a.version == lx.writes && b.version == ly.writes &&
            a.value == x && b.value == y && a.parent == b.parent &&
            a.source != b.source && lx.geometry_port == full && ly.geometry_port == full)
            active_ = a.revision;
    }

    void observe_world_object() {
        if (!active_) {
            ++untagged_;
            return;
        }
        ++tagged_;
        if (scene_ && scene_ != active_) conflict_ = true;
        scene_ = active_;
    }

    DrawEvidence finish_scene() {
        transition_ = conflict_ || (scene_ && tagged_ && prior_ != scene_);
        return {prior_, scene_, tagged_, untagged_, conflict_, transition_ || prior_transition_};
    }

    void discard_draw_history() {
        prior_ = active_ = scene_ = 0;
        tagged_ = untagged_ = 0;
        conflict_ = transition_ = prior_transition_ = false;
    }

private:
    struct FocusWord {
        uint64_t version{}, revision{};
        uint32_t value{}, source{}, parent{};
    };
    std::vector<FocusWord> words_;
    uint64_t revision_{}, prior_{}, active_{}, scene_{};
    unsigned tagged_{}, untagged_{};
    uint16_t mode_ = 256;
    bool conflict_{}, prior_transition_{}, transition_{};
};

} // namespace rt
