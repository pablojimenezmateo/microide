#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <functional>
#include <string>
#include <utility>

#include "plugin/PluginThreadTypes.h"
#include "util/MainThreadMailbox.h"
#include "util/SerialWorkQueue.h"

namespace microide::plugin {

// Dedicated worker thread that owns all plugin Lua execution. The UI thread
// posts jobs (closures that run on the worker); the worker posts actions back to
// a mailbox drained on the UI thread. Moving every `lua_State` touch onto this
// single thread keeps the UI from ever blocking on a plugin call while preserving
// Lua's single-threaded-per-state requirement (one thread => no per-state locks).
//
// This is pure composition over two shared primitives — a lazy-start serial work
// queue (inbound) and a main-thread mailbox (outbound) — leaving PluginThread to
// own only the Lua-specific invariants:
//   - A posted job is the ONLY place a `lua_State` may be touched, and only on the
//     worker thread.
//   - A PluginMainThreadAction must never capture a `lua_State*` / Lua ref; all Lua
//     extraction happens on the worker before the action is posted.
//
// The worker is spawned lazily on first use (EnsureStarted), so a project with no
// plugins never creates it and the scheduled-wake drain costs one relaxed atomic
// load. When started but idle the worker blocks on a condition variable (zero CPU).
class PluginThread {
 public:
  PluginThread() = default;
  ~PluginThread() = default;

  PluginThread(const PluginThread&) = delete;
  PluginThread& operator=(const PluginThread&) = delete;

  // SDL event pushed to wake the UI loop when a mailbox action is queued. 0
  // disables waking (the drain still runs on the next scheduled wake).
  void SetWakeEventType(Uint32 event_type) { mailbox_.SetWakeEventType(event_type); }

  // Spawn the worker if it is not already running. Idempotent; cheap when started.
  void EnsureStarted() { inbound_.EnsureStarted(); }
  bool started() const { return inbound_.started(); }

  // UI thread -> worker. `Post` runs jobs in FIFO order. `PostLatest` first drops
  // any queued (not yet running) job with the same key so superseded work — a
  // stale hover/completion request — is discarded instead of backing up.
  void Post(std::function<void()> task) { inbound_.Post(std::move(task)); }
  void PostLatest(std::string key, std::function<void()> task) {
    inbound_.PostLatest(std::move(key), std::move(task));
  }
  // Jump the queue: run before any already-queued job (it still waits for the
  // job currently mid-PCall, which the watchdog bounds). For a user-blocking,
  // deadline-bounded round-trip (save participants) that must not sit behind a
  // backlog of speculative query jobs.
  void PostFront(std::function<void()> task) { inbound_.PostFront(std::move(task)); }

  // Worker -> UI thread. Queues an action and wakes the UI loop.
  void PostToMain(PluginMainThreadAction action) { mailbox_.Post(std::move(action)); }

  // UI thread: run every queued mailbox action. Returns the number drained.
  int DrainMainThreadActions() { return mailbox_.Drain(); }

  // UI thread: discard every queued mailbox action WITHOUT running it. Used on
  // project-switch teardown so a previous project's deferred plugin mutations
  // (diagnostics, decorations, surfaces, open_file, …) never drain into the newly
  // active project's state.
  void ClearMainThreadActions() { mailbox_.Clear(); }

  // Lockless fast-path gate for the scheduled-wake poll: non-zero only while
  // mailbox actions await draining.
  int PendingMainThreadActionCount() const { return mailbox_.PendingCount(); }

  // Cancel queued jobs and join the worker, waiting up to `deadline`. The
  // per-call watchdog bounds any in-flight job so the join stays prompt.
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000)) {
    inbound_.Shutdown(deadline);
  }

  // Quiesce the worker without stopping it: cancel queued jobs and wait for the
  // in-flight job to finish. Used by project-switch host teardown, which frees the
  // lua_State on the UI thread and must not race a worker job, yet keeps the worker
  // alive to serve the next project's reload.
  void Drain() { inbound_.Drain(); }

 private:
  // Inbound queue (UI -> worker); lazy so an unused host spawns no thread.
  util::SerialWorkQueue inbound_{util::SerialWorkQueue::StartMode::kLazy};
  // Outbound mailbox (worker -> UI).
  util::MainThreadMailbox mailbox_;
};

}  // namespace microide::plugin
