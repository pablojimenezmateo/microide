#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin/PluginThreadTypes.h"

namespace microide::plugin {

// Dedicated worker thread that owns all plugin Lua execution. The UI thread
// posts jobs (closures that run on the worker); the worker posts actions back to
// a mailbox drained on the UI thread. Moving every `lua_State` touch onto this
// single thread keeps the UI from ever blocking on a plugin call while preserving
// Lua's single-threaded-per-state requirement (one thread => no per-state locks).
//
// The thread is spawned lazily on first use (EnsureStarted), so a project with no
// plugins never creates it and the scheduled-wake drain costs one relaxed atomic
// load. When started but idle the worker blocks on a condition variable (zero CPU).
//
// Invariants:
//   - A posted job is the ONLY place a `lua_State` may be touched, and only on the
//     worker thread.
//   - A PluginMainThreadAction must never capture a `lua_State*` / Lua ref; all Lua
//     extraction happens on the worker before the action is posted.
class PluginThread {
 public:
  PluginThread() = default;
  ~PluginThread();

  PluginThread(const PluginThread&) = delete;
  PluginThread& operator=(const PluginThread&) = delete;

  // SDL event pushed to wake the UI loop when a mailbox action is queued. 0
  // disables waking (the drain still runs on the next scheduled wake).
  void SetWakeEventType(Uint32 event_type);

  // Spawn the worker if it is not already running. Idempotent; cheap when started.
  void EnsureStarted();
  bool started() const { return started_.load(std::memory_order_acquire); }

  // UI thread -> worker. `Post` runs jobs in FIFO order. `PostLatest` first drops
  // any queued (not yet running) job with the same key so superseded work — a
  // stale hover/completion request — is discarded instead of backing up.
  void Post(std::function<void()> task);
  void PostLatest(std::string key, std::function<void()> task);
  // Jump the queue: run before any already-queued job (it still waits for the
  // job currently mid-PCall, which the watchdog bounds). For a user-blocking,
  // deadline-bounded round-trip (save participants) that must not sit behind a
  // backlog of speculative query jobs.
  void PostFront(std::function<void()> task);

  // Worker -> UI thread. Queues an action and wakes the UI loop.
  void PostToMain(PluginMainThreadAction action);

  // UI thread: run every queued mailbox action. Returns the number drained.
  int DrainMainThreadActions();

  // Lockless fast-path gate for the scheduled-wake poll: non-zero only while
  // mailbox actions await draining.
  int PendingMainThreadActionCount() const {
    return mailbox_queued_.load(std::memory_order_acquire);
  }

  // Cancel queued jobs and join the worker, waiting up to `deadline`. The
  // per-call watchdog bounds any in-flight job so the join stays prompt.
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

 private:
  void WorkerMain();

  struct Job {
    std::string key;  // empty = no dedup
    std::function<void()> task;
    bool cancelled = false;
  };

  // Inbound queue (UI -> worker).
  std::mutex inbound_mutex_;
  std::condition_variable inbound_cv_;
  std::deque<Job> inbound_;
  std::thread worker_;
  std::atomic<bool> started_{false};
  bool stop_ = false;

  // Outbound mailbox (worker -> UI).
  std::mutex mailbox_mutex_;
  std::vector<PluginMainThreadAction> mailbox_;
  std::atomic<int> mailbox_queued_{0};
  std::atomic<Uint32> wake_event_type_{0};
};

}  // namespace microide::plugin
