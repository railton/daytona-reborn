// Standalone test for ../tools/common/gpu_submission.h with a mock backend (no SDL, no network).
#include "../tools/common/gpu_submission.h"
#include <cstdio>
#include <cstring>
#include <utility>

using namespace rt;

struct Log {  // fixed buffer, no allocation
  char b[32] = {0}; int n = 0;
  Log &operator+=(char c) noexcept { if (n < 31) b[n++] = c; return *this; }
  bool operator==(const char *s) const noexcept { return std::strcmp(b, s) == 0; }
};
struct Mock {
  using Command = int *;
  using Fence = int *;
  int fa = 1, fb = 2;
  bool cancel_ok = true, null_fence = false, wait_ok = true;
  int cancels = 0, submits = 0, waits = 0, idles = 0, releases = 0, last_count = 0;
  Log log;
  bool cancel(Command) noexcept { ++cancels; log += 'C'; return cancel_ok; }
  Fence submit(Command) noexcept { ++submits; log += 'S'; return null_fence ? nullptr : (submits == 1 ? &fa : &fb); }
  bool wait(Fence *, int n, bool) noexcept { ++waits; last_count = n; log += 'W'; return wait_ok; }
  void release_fence(Fence) noexcept { ++releases; log += 'R'; }
  void wait_idle() noexcept { ++idles; log += 'I'; }
};
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static int cmdA, cmdB;

int main() {
  { // nominal
    Mock m; { GpuSubmission<Mock> s(m);
      CHECK(s.begin(&cmdA)); CHECK(s.submit()); CHECK(s.wait()); CHECK(s.wait());
      CHECK(s.settle()); CHECK(s.settle()); CHECK(s.proven()); }
    CHECK(m.log == "SWR"); CHECK(m.waits == 1 && m.idles == 0 && m.cancels == 0); }
  { // cancel=true, never submitted
    Mock m; { GpuSubmission<Mock> s(m); CHECK(s.begin(&cmdA)); CHECK(s.settle()); CHECK(s.settle()); }
    CHECK(m.log == "C"); CHECK(m.idles == 0 && m.releases == 0); }
  { // cancel=false: uncertain, idle fallback, stable false
    Mock m; m.cancel_ok = false; GpuSubmission<Mock> s(m);
    CHECK(s.begin(&cmdA)); CHECK(!s.settle()); CHECK(!s.settle()); CHECK(!s.proven());
    CHECK(m.log == "CI"); CHECK(!s.begin(&cmdA)); }
  { // submit without fence: false, settle waits idle, no backend wait on null fence
    Mock m; m.null_fence = true; GpuSubmission<Mock> s(m);
    CHECK(s.begin(&cmdA)); CHECK(!s.submit()); CHECK(!s.wait()); CHECK(!s.settle()); CHECK(!s.settle());
    CHECK(m.log == "SI"); CHECK(m.waits == 0 && m.idles == 1 && m.releases == 0); }
  { // wait=false: uncertain, idle once, fence released after, stable false
    Mock m; m.wait_ok = false; GpuSubmission<Mock> s(m);
    CHECK(s.begin(&cmdA)); CHECK(s.submit()); CHECK(!s.wait()); CHECK(!s.wait());
    CHECK(!s.settle()); CHECK(!s.settle()); CHECK(m.log == "SWIR"); }
  { // submit without wait, destructor settles: waits then releases
    Mock m; { GpuSubmission<Mock> s(m); s.begin(&cmdA); s.submit(); }
    CHECK(m.log == "SWR"); }
  { // misuse: double begin, submit from Idle, null command
    Mock m; GpuSubmission<Mock> s(m);
    CHECK(!s.submit()); CHECK(!s.begin(nullptr)); CHECK(s.begin(&cmdA)); CHECK(!s.begin(&cmdB));
    CHECK(s.state() == SubmissionState::Recording); CHECK(s.settle()); CHECK(!s.begin(&cmdA)); }
  { // group: two submits, ONE wait for both, no second wait at settle
    Mock m; { GpuSubmission<Mock> a(m), b(m);
      CHECK(a.begin(&cmdA)); CHECK(a.submit()); CHECK(b.begin(&cmdB)); CHECK(b.submit());
      CHECK(wait_group(a, b)); CHECK(m.waits == 1 && m.last_count == 2);
      CHECK(a.state() == SubmissionState::Completed && b.state() == SubmissionState::Completed);
      CHECK(a.wait() && b.wait()); CHECK(a.settle() && b.settle()); }
    CHECK(m.waits == 1); CHECK(m.releases == 2); CHECK(m.idles == 0); CHECK(m.log == "SSWRR"); }
  { // group wait fails: both uncertain, each falls back to idle, stable false
    Mock m; m.wait_ok = false; GpuSubmission<Mock> a(m), b(m);
    a.begin(&cmdA); a.submit(); b.begin(&cmdB); b.submit();
    CHECK(!wait_group(a, b)); CHECK(!a.settle()); CHECK(!b.settle()); CHECK(!a.settle());
    CHECK(m.idles == 2); CHECK(m.releases == 2); }
  { // group refused when one pass is not submitted: no backend wait; B cancelled, A proven alone
    Mock m; GpuSubmission<Mock> a(m), b(m);
    a.begin(&cmdA); a.submit(); b.begin(&cmdB);
    CHECK(!wait_group(a, b)); CHECK(m.waits == 0);
    CHECK(b.settle()); CHECK(a.settle()); CHECK(m.log == "SCWR"); }
  { // group refused when one submit had no fence
    Mock m; GpuSubmission<Mock> a(m), b(m); a.begin(&cmdA); a.submit();
    m.null_fence = true; b.begin(&cmdB); CHECK(!b.submit());
    CHECK(!wait_group(a, b)); CHECK(m.waits == 0); CHECK(a.settle()); CHECK(!b.settle()); }
  { // R2: same object twice (equal fences) rejected, no wait, state untouched
    Mock m; GpuSubmission<Mock> a(m); a.begin(&cmdA); a.submit();
    CHECK(!wait_group(a, a)); CHECK(m.waits == 0); CHECK(a.state() == SubmissionState::Submitted);
    CHECK(a.settle()); CHECK(m.log == "SWR"); }
  { // R2: distinct backends rejected, no backend call on either
    Mock m1, m2; GpuSubmission<Mock> a(m1), b(m2);
    a.begin(&cmdA); a.submit(); b.begin(&cmdB); b.submit();
    CHECK(!wait_group(a, b)); CHECK(m1.waits == 0 && m2.waits == 0);
    CHECK(a.state() == SubmissionState::Submitted && b.state() == SubmissionState::Submitted);
    CHECK(a.settle() && b.settle()); CHECK(m1.log == "SWR" && m2.log == "SWR"); }
  { // R3: same backend, EQUAL fences: no wait, both poisoned, ONE owner => ONE release, no resume
    struct EqMock : Mock { Fence submit(Command) noexcept { ++submits; log += 'S'; return &fa; } };
    EqMock m; { GpuSubmission<EqMock> a(m), b(m);
      CHECK(a.begin(&cmdA)); CHECK(a.submit()); CHECK(b.begin(&cmdB)); CHECK(b.submit());
      CHECK(a.fence() == b.fence()); CHECK(!wait_group(a, b)); CHECK(m.waits == 0);
      CHECK(!a.proven() && !b.proven());
      CHECK(b.fence() == nullptr && a.fence() == &m.fa);     // single owner
      CHECK(!a.wait() && !b.wait()); CHECK(m.waits == 0);      // no resume of the wait
      CHECK(!wait_group(a, b)); CHECK(m.waits == 0);           // a second group attempt stays rejected
      CHECK(!a.settle()); CHECK(!b.settle()); CHECK(!a.settle()); }
    CHECK(m.waits == 0); CHECK(m.releases == 1); CHECK(m.idles == 2); CHECK(m.log == "SSIRI"); }
  { // R3: rejected group without mutation (not Submitted): a,b untouched, fences kept
    Mock m; GpuSubmission<Mock> a(m), b(m); a.begin(&cmdA); a.submit(); b.begin(&cmdB);
    CHECK(!wait_group(a, b)); CHECK(a.proven() && b.proven()); CHECK(a.fence() != nullptr);
    CHECK(a.state() == SubmissionState::Submitted && b.state() == SubmissionState::Recording);
    CHECK(a.settle() && b.settle()); }
  { // R3: distinct backends with equal-looking fences: rejected WITHOUT mutation (not poisoned)
    Mock m1, m2; GpuSubmission<Mock> a(m1), b(m2); a.begin(&cmdA); a.submit(); b.begin(&cmdB); b.submit();
    CHECK(a.fence() == &m1.fa && b.fence() == &m2.fa); CHECK(!wait_group(a, b));
    CHECK(a.proven() && b.proven() && a.fence() == &m1.fa && b.fence() == &m2.fa);
    CHECK(a.settle() && b.settle()); CHECK(m1.releases == 1 && m2.releases == 1); }
  { // R3: same object rejected without mutation
    Mock m; GpuSubmission<Mock> a(m); a.begin(&cmdA); a.submit();
    CHECK(!wait_group(a, a)); CHECK(a.proven() && a.fence() == &m.fa && m.waits == 0 && m.idles == 0);
    CHECK(a.settle() && m.releases == 1); }
  { // R3: static contract: all backend ops noexcept (also checked by static_assert in the header)
    static_assert(noexcept(std::declval<Mock &>().submit(nullptr)), "submit noexcept");
    static_assert(noexcept(std::declval<Mock &>().wait_idle()), "wait_idle noexcept"); }  { // R2: individual proof unchanged after a rejected group
    Mock m; GpuSubmission<Mock> a(m), b(m); a.begin(&cmdA); a.submit(); b.begin(&cmdB); b.submit();
    CHECK(!wait_group(a, a)); CHECK(a.wait() && b.wait()); CHECK(m.waits == 2 && m.last_count == 1);
    CHECK(a.settle() && b.settle()); }
  std::printf(failures ? "FAILED: %d\n" : "OK all checks passed\n", failures);
  return failures ? 1 : 0;
}
