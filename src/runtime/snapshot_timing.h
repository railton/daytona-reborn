#ifndef RT_RUNTIME_SNAPSHOT_TIMING_H_
#define RT_RUNTIME_SNAPSHOT_TIMING_H_

#include <cmath>
#include <cstdint>
#include <optional>

namespace rt {

struct SnapshotStamp {
  std::uint64_t tick = 0;
  std::uint64_t time_ns = 0;
  // True only when an explicit discontinuity (cut/teleport/reset) separates
  // this stamp from the preceding one. Set by the producer, never inferred.
  bool discontinuity_before = false;
  // Distinct GameLoops need distinct IDs within the consumer's history.
  std::uint64_t session_id = 0;
};

// frames is the count after a completed step. The caller supplies its
// observation time in monotonic nanoseconds, in the presentation clock domain
// if these stamps will be used for blending.
inline std::optional<SnapshotStamp> completed_snapshot_stamp(
    std::uint64_t frames, std::uint64_t time_ns, std::uint64_t session_id) {
  if (frames == 0) return std::nullopt;
  return SnapshotStamp{frames - 1, time_ns, frames == 1, session_id};
}

struct SnapshotBlend {
  bool can_interpolate = false;
  double alpha = 1.0;
};

inline SnapshotBlend sample_snapshots(SnapshotStamp previous,
                                      SnapshotStamp next,
                                      std::uint64_t presentation_ns,
                                      std::uint64_t delay_ns) {
  // Rendering one snapshot interval behind (presentation_ns - delay_ns) masks
  // partially simulated state; this is the intended one-snapshot latency.
  // This is only a timing primitive: it neither mutates simulation state nor
  // proves that distinct frames were rendered.
  const bool adjacent = previous.tick != UINT64_MAX &&
                        next.tick == previous.tick + 1;
  // Only the newer stamp marks its preceding edge; previous.discontinuity_before
  // concerns an older edge and is intentionally ignored.
  if (!adjacent || previous.session_id != next.session_id ||
      next.discontinuity_before ||
      next.time_ns <= previous.time_ns) {
    return {false, 1.0};  // Invalid interval: use the latest snapshot.
  }

  const std::uint64_t sample_ns =
      presentation_ns > delay_ns ? presentation_ns - delay_ns : 0;
  if (sample_ns <= previous.time_ns) return {true, 0.0};
  if (sample_ns >= next.time_ns) return {true, 1.0};

  // Subtract integer timestamps first so large epoch values above 2^53 do
  // not lose their small (e.g. sub-microsecond) differences in double.
  const double elapsed = static_cast<double>(sample_ns - previous.time_ns);
  const double span = static_cast<double>(next.time_ns - previous.time_ns);
  double alpha = elapsed / span;

  // This sample is strictly interior, but for large uint64 timestamps the
  // converted elapsed and span can become the same double.  Keep such an
  // interior sample distinct from the exact endpoint clamps above.
  if (alpha <= 0.0) {
    alpha = std::nextafter(0.0, 1.0);
  } else if (alpha >= 1.0) {
    alpha = std::nextafter(1.0, 0.0);
  }
  return {true, alpha};
}

}  // namespace rt

#endif  // RT_RUNTIME_SNAPSHOT_TIMING_H_
