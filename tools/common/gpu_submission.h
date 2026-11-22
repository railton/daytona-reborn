#ifndef RT_GPU_SUBMISSION_H_
#define RT_GPU_SUBMISSION_H_

// Owns a single GPU command buffer and its fence. Backend supplies the GPU calls:
//   using Command = ...; using Fence = ...;           // nullable handles (== nullptr)
//   bool  cancel(Command);                            // true = cancel proven
//   Fence submit(Command);                            // nullptr = no fence returned
//   bool  wait(Fence *fences, int count, bool all);   // true = wait proven
//   void  release_fence(Fence);
//   void  wait_idle();                                // last resort before release
// All backend operations (cancel, submit, wait, release_fence, wait_idle) MUST be
// noexcept. This is enforced at compile time (static_assert on noexcept(...) in GpuSubmission),
// so submit() can never throw after the state change and leave a misleading proven()==true.
// A backend that can fail reports it through the results only (null fence, false).
// FENCE UNIQUENESS: while a fence returned by submit() is alive (not yet released), the backend
// MUST NOT return an equal fence for another submission: each live fence has exactly ONE owner
// submission, and only that owner calls release_fence on it. Defense in depth: if wait_group
// observes equal fences from the same backend (contract violation), it poisons both submissions,
// performs NO backend wait, and keeps a single owner (a) of the fence; b drops its copy without
// releasing, so release happens once. Both then settle through wait_idle only.
// Textures / transfer buffers are NOT held here and no releaser list exists: they stay in
// the pass object, which MUST declare its Submission member AFTER them. Members are destroyed
// in reverse order, so ~Submission (-> settle) runs first and the resources are released
// only once the command was cancelled or its fence waited (fail closed otherwise).
//
// Abandon order in settle(), always: cancel recording command -> wait fence if a submit was
// attempted (even with a null fence) -> wait_idle only if any proof is missing -> release fence.
// After uncertainty the result stays false for the lifetime of the object.

#include <cstddef>
#include <utility>

namespace rt {

enum class SubmissionState { Idle, Recording, Submitted, Completed, Settled };

template <class Backend>
class GpuSubmission {
 public:
  using Command = typename Backend::Command;
  using Fence = typename Backend::Fence;

  static_assert(noexcept(std::declval<Backend &>().cancel(std::declval<Command>())), "cancel must be noexcept");
  static_assert(noexcept(std::declval<Backend &>().submit(std::declval<Command>())), "submit must be noexcept");
  static_assert(noexcept(std::declval<Backend &>().wait(std::declval<Fence *>(), 0, true)), "wait must be noexcept");
  static_assert(noexcept(std::declval<Backend &>().release_fence(std::declval<Fence>())), "release_fence must be noexcept");
  static_assert(noexcept(std::declval<Backend &>().wait_idle()), "wait_idle must be noexcept");

  explicit GpuSubmission(Backend &backend) : backend_(backend) {}
  GpuSubmission(const GpuSubmission &) = delete;
  GpuSubmission &operator=(const GpuSubmission &) = delete;
  ~GpuSubmission() { settle(); }

  SubmissionState state() const { return state_; }
  bool proven() const { return !uncertain_; }  // false once any proof is missing (sticky)
  Fence fence() const { return fence_; }

  // Idle -> Recording. Rejected (false, no change) for a null command or a non-idle owner.
  bool begin(Command cmd) {
    if (state_ != SubmissionState::Idle || cmd == Command{}) return false;
    cmd_ = cmd;
    state_ = SubmissionState::Recording;
    return true;
  }

  // Recording -> Submitted. submit_attempted_ is set BEFORE the call: a null fence does not
  // prove "not submitted". Null fence => false, uncertain; settle() will still wait idle.
  bool submit() {
    if (state_ != SubmissionState::Recording) return false;
    Command c = cmd_;
    cmd_ = Command{};  // ownership passes to the backend whatever submit returns
    submit_attempted_ = true;
    state_ = SubmissionState::Submitted;
    fence_ = backend_.submit(c);
    if (fence_ == Fence{}) { uncertain_ = true; return false; }
    return true;
  }

  // Own wait on this pass's fence. Submitted -> Completed. Idempotent: a second call after
  // success returns true without touching the backend; after failure it returns false.
  bool wait() {
    if (state_ == SubmissionState::Completed) return !uncertain_;
    if (state_ != SubmissionState::Submitted || fence_ == Fence{} || uncertain_) return false;
    const bool ok = backend_.wait(&fence_, 1, true);
    return finish_wait(ok);
  }

  // Abandon / finish. Idempotent, noexcept, stable result: true only if every step was proven.
  bool settle() noexcept {
    if (state_ == SubmissionState::Settled) return !uncertain_;
    if (cmd_ != Command{}) {
      if (!backend_.cancel(cmd_)) uncertain_ = true;
      cmd_ = Command{};
    }
    if (submit_attempted_) {  // not yet proven by wait()/group wait
      if (fence_ == Fence{} || !backend_.wait(&fence_, 1, true)) uncertain_ = true;
      submit_attempted_ = false;
    }
    if (uncertain_) backend_.wait_idle();
    if (fence_ != Fence{}) { backend_.release_fence(fence_); fence_ = Fence{}; }
    state_ = SubmissionState::Settled;
    return !uncertain_;
  }

 private:
  template <class B> friend bool wait_group(GpuSubmission<B> &, GpuSubmission<B> &);

  // Finalisation after a shared group wait: reachable only from wait_group (friend). Never
  // calls the backend.
  bool finalize_group_wait(bool group_ok) {
    if (state_ == SubmissionState::Completed) return !uncertain_;
    if (state_ != SubmissionState::Submitted) return false;
    return finish_wait(group_ok && fence_ != Fence{});
  }

  // Equal-fence defense (wait_group only). Owner keeps the fence (released once by settle);
  // the other drops its copy. Neither waits on the fence: settle() goes straight to wait_idle.
  void poison_shared_fence_owner() { uncertain_ = true; submit_attempted_ = false; }
  void poison_shared_fence_owner_drop() { poison_shared_fence_owner(); fence_ = Fence{}; }

  bool finish_wait(bool ok) {
    if (ok) {
      submit_attempted_ = false;  // proof obtained: settle() must not wait again
      state_ = SubmissionState::Completed;
      return !uncertain_;
    }
    submit_attempted_ = false;  // the wait was already tried: no second wait, settle() goes to idle
    uncertain_ = true;
    return false;
  }

  Backend &backend_;
  Command cmd_{};
  Fence fence_{};
  SubmissionState state_ = SubmissionState::Idle;
  bool submit_attempted_ = false;
  bool uncertain_ = false;
};

// One backend wait for two submissions (all=true), then finalisation of both. The backend is
// taken from the submissions themselves: both must refer to the SAME backend instance (address
// identity), so no external backend argument exists. Rejected (false, NO backend call) when:
// a and b are the same object, backends differ, or either is not Submitted/proven/fenced: these
// leave both submissions UNCHANGED. Equal fences from the same backend are also rejected with no
// backend call, but poison both (see FENCE UNIQUENESS) instead of leaving them untouched. Proof contract:
// a failed group wait marks both uncertain, so both fall back to wait_idle on settle.
template <class Backend>
bool wait_group(GpuSubmission<Backend> &a, GpuSubmission<Backend> &b) {
  using Fence = typename Backend::Fence;
  if (&a == &b || &a.backend_ != &b.backend_) return false;
  if (a.state_ != SubmissionState::Submitted || b.state_ != SubmissionState::Submitted)
    return false;
  if (a.uncertain_ || b.uncertain_ || a.fence_ == Fence{} || b.fence_ == Fence{}) return false;
  if (a.fence_ == b.fence_) {  // contract violation: poison both, no wait, single fence owner
    a.poison_shared_fence_owner();
    b.poison_shared_fence_owner_drop();
    return false;
  }
  Fence fences[2] = {a.fence_, b.fence_};
  const bool ok = a.backend_.wait(fences, 2, true);
  const bool ra = a.finalize_group_wait(ok);
  const bool rb = b.finalize_group_wait(ok);
  return ok && ra && rb;
}

}  // namespace rt

#endif  // RT_GPU_SUBMISSION_H_
