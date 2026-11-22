#include "runtime/snapshot_timing.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

// Not assert(): these checks must stay active when NDEBUG is defined.
void check(bool ok, const char* expr, int line) {
  if (!ok) {
    std::fprintf(stderr, "check failed at line %d: %s\n", line, expr);
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool near(double lhs, double rhs) {
  return std::fabs(lhs - rhs) <= 1e-12;
}

void expect_latest(rt::SnapshotStamp previous, rt::SnapshotStamp next) {
  const rt::SnapshotBlend blend =
      rt::sample_snapshots(previous, next, 500, 0);
  CHECK(!blend.can_interpolate);
  CHECK(blend.alpha == 1.0);
}

// Synthetic positions for the offline renderer check.
double synthetic_position(const rt::SnapshotBlend& blend) {
  return std::lerp(1.0, 5.0, blend.alpha);
}

void emit_synthetic_positions() {
  using rt::SnapshotStamp;
  const SnapshotStamp previous{10, 1000, false, 7};
  const SnapshotStamp next{11, 2000, false, 7};
  const SnapshotStamp cut{11, 2000, true, 7};
  const SnapshotStamp cross_session{11, 2000, false, 8};

  const auto start = rt::sample_snapshots(previous, next, 1000, 0);
  const auto half = rt::sample_snapshots(previous, next, 1500, 0);
  const auto end = rt::sample_snapshots(previous, next, 2000, 0);
  CHECK(start.can_interpolate && half.can_interpolate && end.can_interpolate);
  const auto cut_midpoint = rt::sample_snapshots(previous, cut, 1500, 0);
  const auto cross_midpoint = rt::sample_snapshots(previous, cross_session, 1500, 0);
  CHECK(!cut_midpoint.can_interpolate && cut_midpoint.alpha == 1.0);
  CHECK(!cross_midpoint.can_interpolate && cross_midpoint.alpha == 1.0);

  std::printf(
      "{\"continuous\":[%.17g,%.17g,%.17g],\"cut_midpoint\":%.17g,"
      "\"cross_session_midpoint\":%.17g}\n",
      synthetic_position(start), synthetic_position(half), synthetic_position(end),
      synthetic_position(cut_midpoint), synthetic_position(cross_midpoint));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--emit-synthetic-positions") == 0) {
    emit_synthetic_positions();
    return EXIT_SUCCESS;
  }
  if (argc != 1) {
    std::fprintf(stderr, "usage: %s [--emit-synthetic-positions]\n", argv[0]);
    return 2;
  }

  using rt::SnapshotStamp;
  const SnapshotStamp a{10, 1000};
  const SnapshotStamp b{11, 2000};

  const auto before = rt::sample_snapshots(a, b, 999, 0);
  CHECK(before.can_interpolate);
  CHECK(near(before.alpha, 0.0));

  const auto inside = rt::sample_snapshots(a, b, 1250, 0);
  CHECK(inside.can_interpolate);
  CHECK(near(inside.alpha, 0.25));

  const auto after = rt::sample_snapshots(a, b, 2001, 0);
  CHECK(after.can_interpolate);
  CHECK(near(after.alpha, 1.0));

  const auto delayed = rt::sample_snapshots(a, b, 1750, 500);
  CHECK(delayed.can_interpolate);
  CHECK(near(delayed.alpha, 0.25));

  const auto underflow = rt::sample_snapshots(a, b, 250, 500);
  CHECK(underflow.can_interpolate);
  CHECK(near(underflow.alpha, 0.0));

  expect_latest(a, SnapshotStamp{10, 2000});  // duplicate tick
  expect_latest(a, SnapshotStamp{12, 2000});  // gapped tick
  expect_latest(a, SnapshotStamp{9, 2000});   // reversed tick
  expect_latest(a, SnapshotStamp{11, 1000});  // reversed timestamp
  expect_latest(SnapshotStamp{0, 0}, SnapshotStamp{1, 0});  // zero span

  const auto from_zero =
      rt::sample_snapshots(SnapshotStamp{0, 0}, SnapshotStamp{1, 10}, 5, 0);
  CHECK(from_zero.can_interpolate);
  CHECK(near(from_zero.alpha, 0.5));

  const SnapshotStamp near_max_tick{UINT64_MAX - 1, UINT64_MAX - 10};
  const SnapshotStamp max_tick{UINT64_MAX, UINT64_MAX - 5};
  const auto max_step =
      rt::sample_snapshots(near_max_tick, max_tick, UINT64_MAX - 8, 0);
  CHECK(max_step.can_interpolate);
  CHECK(near(max_step.alpha, 0.4));

  // A max tick must not appear to wrap to tick zero.
  expect_latest(SnapshotStamp{UINT64_MAX, 0}, SnapshotStamp{0, 1});

  const auto max_time =
      rt::sample_snapshots(SnapshotStamp{30, UINT64_MAX - 1},
                           SnapshotStamp{31, UINT64_MAX}, UINT64_MAX, 0);
  CHECK(max_time.can_interpolate);
  CHECK(near(max_time.alpha, 1.0));

  const auto max_delay = rt::sample_snapshots(a, b, 1000, UINT64_MAX);
  CHECK(max_delay.can_interpolate);
  CHECK(near(max_delay.alpha, 0.0));

  // Epoch values above 2^53 with a sub-microsecond (3 ns) interval.
  const std::uint64_t large = (std::uint64_t{1} << 53) + 1000;
  const auto above_2_53 = rt::sample_snapshots(
      SnapshotStamp{40, large}, SnapshotStamp{41, large + 3}, large + 1, 0);
  CHECK(above_2_53.can_interpolate);
  CHECK(near(above_2_53.alpha, 1.0 / 3.0));

  // Regression: previous {1, 0}, next {2, 9007199254740993}, presented at
  // 9007199254740992 with no delay.  Both timestamps round to the same double,
  // which previously made this strictly interior sample evaluate to 1.0.
  const std::uint64_t regression_presentation = std::uint64_t{1} << 53;
  const auto rounded_span = rt::sample_snapshots(
      SnapshotStamp{1, 0}, SnapshotStamp{2, regression_presentation + 1},
      regression_presentation, 0);
  CHECK(rounded_span.can_interpolate);
  CHECK(rounded_span.alpha > 0.0);
  CHECK(rounded_span.alpha < 1.0);

  // The exact endpoint clamps must remain exact for the same interval.
  const auto clamped_before = rt::sample_snapshots(
      SnapshotStamp{1, 0}, SnapshotStamp{2, regression_presentation + 1}, 0,
      0);
  CHECK(clamped_before.can_interpolate);
  CHECK(clamped_before.alpha == 0.0);

  const auto clamped_after = rt::sample_snapshots(
      SnapshotStamp{1, 0}, SnapshotStamp{2, regression_presentation + 1},
      regression_presentation + 1, 0);
  CHECK(clamped_after.can_interpolate);
  CHECK(clamped_after.alpha == 1.0);

  // Explicit discontinuity checks.
  // Default is unmarked: existing aggregate-init behavior unchanged.
  CHECK(!SnapshotStamp{}.discontinuity_before);
  CHECK(!a.discontinuity_before);

  // Adjacent, increasing timestamps still fail across a marked edge,
  // whatever the sample time (before, inside, after).
  const SnapshotStamp cut{11, 2000, true};
  for (std::uint64_t t : {std::uint64_t{0}, std::uint64_t{1000},
                          std::uint64_t{1500}, std::uint64_t{2000},
                          std::uint64_t{5000}}) {
    const auto blocked = rt::sample_snapshots(a, cut, t, 0);
    CHECK(!blocked.can_interpolate);
    CHECK(blocked.alpha == 1.0);
  }
  const auto blocked_delay = rt::sample_snapshots(a, cut, 1750, 500);
  CHECK(!blocked_delay.can_interpolate);
  CHECK(blocked_delay.alpha == 1.0);

  // Only the newer stamp counts: a marked previous does not block the
  // following unmarked edge, which blends normally.
  const SnapshotStamp marked_prev{11, 2000, true};
  const SnapshotStamp next_unmarked{12, 3000};
  const auto after_cut = rt::sample_snapshots(marked_prev, next_unmarked, 2250, 0);
  CHECK(after_cut.can_interpolate);
  CHECK(near(after_cut.alpha, 0.25));

  // Exact endpoints on an unmarked edge after a marked previous stamp.
  const auto ep0 = rt::sample_snapshots(marked_prev, next_unmarked, 2000, 0);
  CHECK(ep0.can_interpolate);
  CHECK(ep0.alpha == 0.0);
  const auto ep1 = rt::sample_snapshots(marked_prev, next_unmarked, 3000, 0);
  CHECK(ep1.can_interpolate);
  CHECK(ep1.alpha == 1.0);

  // Marked previous with unmarked next is identical to fully unmarked.
  const auto plain = rt::sample_snapshots(SnapshotStamp{11, 2000},
                                          SnapshotStamp{12, 3000}, 2250, 0);
  CHECK(plain.can_interpolate && plain.alpha == after_cut.alpha);

  // The mark never rescues an otherwise invalid edge.
  expect_latest(a, SnapshotStamp{12, 2000, false});
  expect_latest(a, SnapshotStamp{12, 2000, true});
  expect_latest(a, SnapshotStamp{11, 1000, true});

  // A fresh loop has no completed tick, and the first completed tick is zero.
  CHECK(!rt::completed_snapshot_stamp(0, 1000, 7));
  const auto first = rt::completed_snapshot_stamp(1, 1000, 7);
  const auto second = rt::completed_snapshot_stamp(2, 2000, 7);
  CHECK(first && first->tick == 0 && first->time_ns == 1000);
  CHECK(first->discontinuity_before && first->session_id == 7);
  CHECK(second && second->tick == 1 && second->time_ns == 2000);
  CHECK(!second->discontinuity_before && second->session_id == 7);
  CHECK(rt::sample_snapshots(*first, *second, 1500, 0).can_interpolate);

  // The observer missed tick zero of the replacement session. The stamps look
  // adjacent, with increasing times and no mark on the newer stamp.
  const auto replacement = rt::completed_snapshot_stamp(2, 2000, 8);
  CHECK(replacement && !replacement->discontinuity_before);
  const auto cross_session = rt::sample_snapshots(*first, *replacement, 1500, 0);
  CHECK(!cross_session.can_interpolate && cross_session.alpha == 1.0);

  return EXIT_SUCCESS;
}
