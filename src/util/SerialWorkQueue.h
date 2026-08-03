#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace microide::util {

// A single dedicated worker thread draining a FIFO job queue. This is the one
// shared implementation behind the plugin worker's inbound queue (lazy start, no
// accounting) and the project background executor (eager start, with
// BackgroundTaskCounter accounting wired through Hooks).
//
// Posting verbs:
//   - Post:       FIFO, runs after everything already queued.
//   - PostLatest: drops any queued (not yet running) job with the same key first,
//                 so superseded work — a stale hover/blame request — is discarded
//                 instead of backing up. A job already mid-flight is NOT dropped.
//                 The dedup is O(1): a key->node index finds the superseded job
//                 without scanning, and the replacement is appended at the TAIL
//                 (so a plain Post enqueued between two same-key PostLatest calls
//                 stays ahead of the later one — ordered edits before a later
//                 coalesced move, per the plugin cursor/edit contract).
//   - PostFront:  jumps ahead of every queued job (it still waits for the job
//                 currently running). For a user-blocking, deadline-bounded
//                 round-trip that must not sit behind a backlog.
//
// Backpressure. A queue owner may set Limits::max_depth to bound how deep the
// backlog grows (an approximate retained-closure budget — one bounded job count
// keeps retained closures bounded). Only jobs a caller explicitly marks
// Shedding::kDroppable are ever shed; a critical job (the default) is never
// dropped, because a lost WorkspaceEdit apply / save participant is a correctness
// loss, not a stale query. When the queue is at budget, admitting any job first
// sheds the OLDEST droppable queued job (keeping the newest speculative work and
// all critical work). max_depth == 0 (the default) disables the budget entirely.
//
// The worker has no timed join: Shutdown() marks queued jobs cancelled and blocks
// in join(). Callers keep in-flight jobs bounded (plugin Lua watchdog; cooperative
// git/file IO) so the join stays prompt; a timed join would require detaching a
// worker that still touches `this`, which is unsafe.
class SerialWorkQueue {
 public:
  enum class StartMode { kLazy, kEager };

  // Per-job backpressure classification. kCritical (the default) is never shed;
  // kDroppable may be dropped to hold the queue at Limits::max_depth.
  enum class Shedding { kCritical, kDroppable };

  // Optional per-queue backpressure budget. max_depth == 0 => unbounded.
  struct Limits {
    std::size_t max_depth = 0;
  };

  // Accounting hooks. Both null (the default) cost one predictable branch.
  // Contract: on_complete fires EXACTLY ONCE per admitted job, balancing
  // on_enqueue, on every exit path — normal run, cancelled-skip, dropped by a
  // PostLatest dedup, shed under the depth budget, and drained while shutting
  // down. This is what keeps a global in-flight counter balanced.
  struct Hooks {
    std::function<void()> on_enqueue;
    std::function<void()> on_complete;
  };

  explicit SerialWorkQueue(StartMode start_mode = StartMode::kLazy, Hooks hooks = {},
                           Limits limits = Limits{0});
  ~SerialWorkQueue();

  SerialWorkQueue(const SerialWorkQueue&) = delete;
  SerialWorkQueue& operator=(const SerialWorkQueue&) = delete;

  // Spawn the worker if it is not already running. Idempotent; cheap when started.
  // A kEager queue calls this from its constructor; a kLazy queue stays threadless
  // until the first EnsureStarted()/Post* — every Post*/PostLatest/PostFront starts
  // the worker before enqueuing, so a default-constructed queue that is only ever
  // Post()ed to still runs its work.
  void EnsureStarted();
  bool started() const { return started_.load(std::memory_order_acquire); }

  void Post(std::function<void()> task, Shedding shedding = Shedding::kCritical);
  void PostLatest(std::string key, std::function<void()> task,
                  Shedding shedding = Shedding::kCritical);
  void PostFront(std::function<void()> task);

  // Mark all queued (not in-flight) jobs cancelled. The worker stays alive; the
  // cancelled jobs are still popped (and still fire on_complete) but not run.
  void Cancel();

  // Block until the worker is idle WITHOUT stopping it: cancel every queued job,
  // then wait for the job currently mid-flight (if any) plus a trailing barrier to
  // drain. On return the worker is quiescent but still accepts new jobs. This is
  // the teardown-before-reload seam: a lua_State is about to be destroyed on the
  // caller's thread and no worker job may be touching it, yet the same worker must
  // serve the next project's reload (so a permanent Shutdown() is wrong here).
  // Must be called from a thread other than the worker. No-op if never started.
  void Drain();

  // Block until every job queued at the time of the call has RUN. The opposite of
  // Drain(), which reaches idle by cancelling the backlog: Flush() reaches idle by
  // completing it. Use this when queued work must not be lost -- a persisted-state
  // writer whose queue is the only place the user's session exists until it lands
  // on disk. Must be called from a thread other than the worker; no-op if never
  // started (nothing was ever queued to lose).
  void Flush();

  // Cancel queued jobs and join the worker, waiting up to `deadline` (see note
  // above on why the join is unconditional in practice).
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

 private:
  void WorkerMain();
  // Spawn the worker assuming mutex_ is already held. EnsureStarted() and the Post*
  // enqueue paths share this so a lazy queue starts on first post without a nested
  // re-lock of mutex_ (which would deadlock).
  void EnsureStartedLocked();

  struct Job {
    std::string key;  // empty = no dedup
    std::function<void()> task;
    bool cancelled = false;
    bool droppable = false;
    // Barrier jobs (Drain) only signal a waiting promise; they must still run when
    // a concurrent Cancel()/Shutdown() flags the whole queue cancelled, or Drain()
    // would block forever on a promise no one ever fulfills.
    bool run_even_if_cancelled = false;
  };

  using JobIter = std::list<Job>::iterator;

  // Erase a queued node, keep the key index consistent, and fire the balancing
  // on_complete for the job that will now never run. mutex_ held; `it` valid.
  void EraseQueuedLocked(JobIter it);
  // If a depth budget is set and the queue is at (or over) it, shed the single
  // oldest droppable, non-cancelled queued job. mutex_ held.
  void ShedIfOverBudgetLocked();
  // Drop every key-index entry (all queued jobs just became cancelled / cleared).
  void ClearKeyIndexLocked() { keyed_index_.clear(); }

  Hooks hooks_;
  Limits limits_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::list<Job> queue_;
  // key -> the queued, non-cancelled node with that key. At most one live node per
  // key. Kept in lockstep with queue_: superseded/popped/cancelled nodes leave the
  // index so PostLatest dedup stays O(1) and never resurrects a stale node.
  std::unordered_map<std::string, JobIter> keyed_index_;
  std::thread worker_;
  std::atomic<bool> started_{false};
  bool stop_ = false;
};

}  // namespace microide::util
