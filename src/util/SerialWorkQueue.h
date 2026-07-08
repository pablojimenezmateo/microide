#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
//   - PostFront:  jumps ahead of every queued job (it still waits for the job
//                 currently running). For a user-blocking, deadline-bounded
//                 round-trip that must not sit behind a backlog.
//
// The worker has no timed join: Shutdown() marks queued jobs cancelled and blocks
// in join(). Callers keep in-flight jobs bounded (plugin Lua watchdog; cooperative
// git/file IO) so the join stays prompt; a timed join would require detaching a
// worker that still touches `this`, which is unsafe.
class SerialWorkQueue {
 public:
  enum class StartMode { kLazy, kEager };

  // Accounting hooks. Both null (the default) cost one predictable branch.
  // Contract: on_complete fires EXACTLY ONCE per admitted job, balancing
  // on_enqueue, on every exit path — normal run, cancelled-skip, dropped by a
  // PostLatest dedup, and drained while shutting down. This is what keeps a
  // global in-flight counter balanced.
  struct Hooks {
    std::function<void()> on_enqueue;
    std::function<void()> on_complete;
  };

  explicit SerialWorkQueue(StartMode start_mode = StartMode::kLazy, Hooks hooks = {});
  ~SerialWorkQueue();

  SerialWorkQueue(const SerialWorkQueue&) = delete;
  SerialWorkQueue& operator=(const SerialWorkQueue&) = delete;

  // Spawn the worker if it is not already running. Idempotent; cheap when started.
  // A kEager queue calls this from its constructor; a kLazy queue stays threadless
  // until the first EnsureStarted()/Post*.
  void EnsureStarted();
  bool started() const { return started_.load(std::memory_order_acquire); }

  void Post(std::function<void()> task);
  void PostLatest(std::string key, std::function<void()> task);
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

  // Cancel queued jobs and join the worker, waiting up to `deadline` (see note
  // above on why the join is unconditional in practice).
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

 private:
  void WorkerMain();

  struct Job {
    std::string key;  // empty = no dedup
    std::function<void()> task;
    bool cancelled = false;
  };

  Hooks hooks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  std::thread worker_;
  std::atomic<bool> started_{false};
  bool stop_ = false;
};

}  // namespace microide::util
