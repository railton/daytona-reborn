#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include "runtime/msc2_companion.h"
#include <unordered_set>

namespace rt {

class OverlayRoleProbe {
public:

    bool bind_program(std::string_view build, std::span<const uint8_t> program) {
        store_ = 0;
        if (program.size() != 0x200000)
            return false;
        msc2::SnapshotFingerprint fingerprint;
        fingerprint.append(program);
        if (build == "daytona" && fingerprint.hash == 0x80bd48533132c74full) {
            store_ = 0x17c10;
            returns_ = {0x1315c, 0x1322c, 0x132d0};
        } else if (build == "daytona93" && fingerprint.hash == 0x3c8bd887c7697148ull) {
            store_ = 0x17990;
            returns_ = {0x12b00, 0x12bd0, 0x12c70};
        }
        return store_ != 0;
    }

    bool header_site(uint32_t addr, uint32_t ip, uint32_t return_address) const {
        return store_ != 0 && addr == 0x800410 && ip == store_ &&
            (return_address == returns_[0] || return_address == returns_[1] || return_address == returns_[2]);
    }

    static const char *capture_path() {
        const char *scene = std::getenv("M2_SCENE_EXPORT");
        const char *roles = std::getenv("M2_SCENE_EXPORT_ROLES");
        return scene && *scene && roles && *roles ? roles : nullptr;
    }

    bool active() const { return store_ != 0; }

    void observe(uint64_t frame, uint32_t word, uint64_t version) {
        observed_headers_[word] = {frame, version};
    }

    void begin_parse(uint64_t frame) {
        commands_.clear();
        command_frame_ = frame;
    }

    void consume(uint32_t word, uint32_t ordinal, uint64_t version) {
        const auto it = observed_headers_.find(word);
        if (it != observed_headers_.end() &&
            it->second.frame <= command_frame_ && command_frame_ - it->second.frame <= 1 &&
            it->second.version == version)
            commands_.insert(ordinal);
    }

    bool overlay(uint64_t frame, uint32_t ordinal) const {
        return command_frame_ == frame && commands_.contains(ordinal);
    }

private:
    uint32_t store_ = 0;
    std::array<uint32_t, 3> returns_{};

    struct Header {
        uint64_t frame;
        uint64_t version;
    };
    std::unordered_map<uint32_t, Header> observed_headers_;
    std::unordered_set<uint32_t> commands_;
    uint64_t command_frame_ = UINT64_MAX;
};

} // namespace rt
