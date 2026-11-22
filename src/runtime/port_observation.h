#ifndef RT_PORT_OBSERVATION_H
#define RT_PORT_OBSERVATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include "runtime/write_provenance.h"

namespace rt {

// Diagnostic observations supplied by a caller. These values describe observed
// port activity only; they are not evidence of caller identity or execution.
struct PortObservation {
    std::uint64_t frame = 0;
    std::uint64_t count = 0;
    std::uint32_t address = 0;
    std::uint32_t lane_mask = 0;
    std::uint32_t data = 0;
    std::uint32_t buffer_cursor = 0;
    std::uint32_t observed_ip = 0;
    bool cpu_state_present = false;
    std::array<std::uint32_t, 32> registers{};
    bool buffer_provenance_available = false;
    bool buffer_write_observed = false;
    std::uint32_t buffer_word_written = 0;
    std::uint64_t buffer_write_count = 0;
};

// Capture a completed geometry-port handler, with CPU fields sampled by the
// caller before entry. The per-word counter joins a write event in this board
// lifetime, never an object identity. Exceptions produce no completed record.
template<class Handler>
void observe_port_write(PortObservation& observation,
                        const BufferWriteProvenance* provenance,
                        std::uint32_t cursor, Handler&& handler) {
    observation.buffer_cursor = cursor;
    observation.buffer_provenance_available = provenance != nullptr;
    observation.buffer_write_observed = false;
    observation.buffer_word_written = 0;
    observation.buffer_write_count = 0;
    const std::uint32_t word = cursor / 4;
    const bool valid = provenance && word < provenance->size();
    const std::uint64_t before = valid ? provenance->at(word).writes : 0;
    std::forward<Handler>(handler)();
    if (!valid) return;
    const auto& after = provenance->at(word);
    if (after.writes == before) return; // register change or ignored upload
    if (before == std::numeric_limits<std::uint64_t>::max() ||
        after.writes != before + 1 || after.geometry_port != 0xffffffffu)
        throw std::runtime_error("unexpected geometry port write provenance");
    observation.buffer_write_observed = true;
    observation.buffer_word_written = word;
    observation.buffer_write_count = after.writes;
}

// Writes port observations as CSV.
//
// Contract:
// - Caller identity is always unknown. Nothing in this writer attributes an
//   observation to a caller.
// - cpu_state_present == true means only that the supplied CPU-state snapshot
//   was present. It does not mean the CPU called this code.
// - sequence is the record order within this writer only. It is not an object
//   identity, and it says nothing about who supplied the observation.
// - observed_ip and count are supplied observations. Do not infer an exact
//   instruction PC from them; the synthetic exact/block counterexample already
//   proves that inference false.
// - When cpu_state_present is false, the writer normalizes observed_ip,
//   observed_count, and all r0..r31 fields to zero. Other supplied fields are
//   still emitted as supplied, except nonwrite event coordinates normalized to0.
//   This avoids emitting a sentinel as CPU evidence.
//
// CSV schema (45 columns):
// sequence,frame,address,lane_mask,data_masked,buffer_cursor,
// cpu_state_present,observed_ip,observed_count,r0..r31,
// buffer_provenance_available,buffer_write_observed,buffer_word_written,buffer_write_count
//
// data_masked is data & lane_mask. cpu_state_present is written as 0 or 1.
// The header is written once, immediately before the first row. Sequence starts
// at 0 and increases by one per successfully written row.
//
// The FILE* is borrowed, never owned or closed, and may be null. All file
// errors are checked and latched: after the first failure, write() returns
// false without writing and flush() also returns false. The writer does not
// create environment files or use global files.
class PortObservationWriter {
public:
    static constexpr const char* kHeader =
        "sequence,frame,address,lane_mask,data_masked,buffer_cursor,"
        "cpu_state_present,observed_ip,observed_count,"
        "r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,"
        "r16,r17,r18,r19,r20,r21,r22,r23,r24,r25,r26,r27,r28,r29,r30,r31,"
        "buffer_provenance_available,buffer_write_observed,buffer_word_written,buffer_write_count";

    static constexpr std::size_t kColumnCount = 45;

    explicit PortObservationWriter(std::FILE* file) noexcept
        : file_(file), failed_(file == nullptr) {}

    PortObservationWriter(const PortObservationWriter&) = delete;
    PortObservationWriter& operator=(const PortObservationWriter&) = delete;

    ~PortObservationWriter() = default;

    bool write(const PortObservation& observation) noexcept {
        if (failed_ || file_ == nullptr) {
            failed_ = true;
            return false;
        }

        if (record_count_ >= kMaxRecords) {
            failed_ = true;
            return false;
        }

        if (observation.buffer_write_observed &&
            (!observation.buffer_provenance_available ||
             observation.buffer_word_written >= 0x8000 ||
             observation.buffer_write_count == 0)) {
            failed_ = true;
            return false;
        }
        if (!header_written_) {
            if (std::fprintf(file_, "%s\n", kHeader) < 0 ||
                std::ferror(file_) != 0) {
                failed_ = true;
                return false;
            }
            header_written_ = true;
        }

        const bool present = observation.cpu_state_present;
        const std::uint32_t observed_ip =
            present ? observation.observed_ip : std::uint32_t{0};
        const std::uint64_t observed_count =
            present ? observation.count : std::uint64_t{0};
        const std::uint32_t data_masked =
            observation.data & observation.lane_mask;

        if (std::fprintf(
                file_, "%llu,%llu,%u,%u,%u,%u,%u,%u,%llu",
                static_cast<unsigned long long>(record_count_),
                static_cast<unsigned long long>(observation.frame),
                static_cast<unsigned>(observation.address),
                static_cast<unsigned>(observation.lane_mask),
                static_cast<unsigned>(data_masked),
                static_cast<unsigned>(observation.buffer_cursor),
                static_cast<unsigned>(present ? 1u : 0u),
                static_cast<unsigned>(observed_ip),
                static_cast<unsigned long long>(observed_count)) < 0 ||
            std::ferror(file_) != 0) {
            failed_ = true;
            return false;
        }

        for (const std::uint32_t reg : observation.registers) {
            const unsigned value =
                static_cast<unsigned>(present ? reg : std::uint32_t{0});
            if (std::fprintf(file_, ",%u", value) < 0 ||
                std::ferror(file_) != 0) {
                failed_ = true;
                return false;
            }
        }

        if (std::fprintf(file_, ",%u,%u,%u,%llu",
                unsigned(observation.buffer_provenance_available),
                unsigned(observation.buffer_write_observed),
                observation.buffer_write_observed ? unsigned(observation.buffer_word_written) : 0u,
                observation.buffer_write_observed ? static_cast<unsigned long long>(observation.buffer_write_count) : 0ULL) < 0 ||
            std::ferror(file_) != 0) {
            failed_ = true;
            return false;
        }
        if (std::fputc('\n', file_) == EOF || std::ferror(file_) != 0) {
            failed_ = true;
            return false;
        }

        ++record_count_;
        return true;
    }

    bool flush() noexcept {
        if (failed_ || file_ == nullptr) {
            failed_ = true;
            return false;
        }
        if (std::fflush(file_) != 0 || std::ferror(file_) != 0) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool failed() const noexcept { return failed_; }

    std::uint64_t records_written() const noexcept { return record_count_; }

private:
    static constexpr std::uint64_t kMaxRecords =
        std::numeric_limits<std::uint64_t>::max();

    std::FILE* file_;
    bool failed_;
    bool header_written_ = false;
    std::uint64_t record_count_ = 0;
};

}  // namespace rt

#endif  // RT_PORT_OBSERVATION_H
