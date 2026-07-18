#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::util {

// Background-thread -> main-thread closure mailbox, drained once per frame and
// woken via an SDL custom event. A worker posts a closure (with all of its data
// already extracted, never a live handle into worker-owned state); the main
// thread runs every queued closure during its scheduled drain.
//
// This is the one shared implementation behind the plugin worker's outbound
// mailbox and the DAP/LSP `ready_callbacks` queues, which were three byte-for-
// byte copies of the same pattern.
//
// Lock order: callers may Post()/PostWithoutWake() while holding their own outer
// mutex (the DAP/LSP clients do, under their protocol mutex). The mailbox mutex
// is therefore always the innermost lock and never calls back into caller code.
class MainThreadMailbox {
 public:
  using Action = std::function<void()>;

  MainThreadMailbox() = default;
  MainThreadMailbox(const MainThreadMailbox&) = delete;
  MainThreadMailbox& operator=(const MainThreadMailbox&) = delete;

  // SDL event pushed to wake the UI loop when an action is queued. 0 disables
  // waking (the drain still runs on the next scheduled wake).
  void SetWakeEventType(Uint32 event_type);

  // Enqueue an action and push one wake event.
  void Post(Action action);

  // Enqueue without waking. For batch sites that push many actions and then wake
  // once via PushWake() so the UI loop sees a single event for the whole batch.
  void PostWithoutWake(Action action);

  // Enqueue a REPLACEABLE action coalesced by `key`: if an action with the same
  // key is still queued (not yet drained), replace it in place with this one (the
  // superseded closure — and whatever payload it captured — is dropped). Used for
  // event classes where only the latest matters per key (e.g. LSP publishDiagnostics
  // per URI, which fully replaces a file's diagnostics), so a chatty server/plugin
  // cannot enqueue many stale capped closures between UI drains. Wakes the loop.
  // TD-2026-07-17A-018.
  void PostLatest(std::string key, Action action);

  // Wake the UI loop without enqueuing. For nudging the loop to re-poll state
  // that is not delivered as a mailbox action (e.g. a worker-thread exit).
  // Returns whether the wake event was actually queued: false when the wake type
  // is unset OR SDL_PushEvent rejected the event (a full event queue). On a
  // rejected push while work is pending, an "undelivered wake" bit is latched so
  // the loop's scheduled poll can retry via RetryWakeIfPending().
  bool PushWake() const;

  // Main thread: run every queued action in FIFO order. Returns the count run.
  int Drain();

  // Drop queued actions without running them (protocol reset / teardown).
  void Clear();

  // Lockless fast-path gate for the scheduled-wake poll: non-zero only while
  // actions await draining.
  int PendingCount() const { return queued_.load(std::memory_order_acquire); }

  // True when a wake push failed while actions were still pending, so the main
  // loop owes this mailbox a wake it never received. Cleared on a successful
  // (re)push or on Drain()/Clear().
  bool HasUndeliveredWake() const {
    return wake_delivery_failed_.load(std::memory_order_acquire);
  }

  // Idempotent retry for the scheduled-wake poll: if work is pending and a prior
  // wake push was lost, attempt the push again. Returns true when the mailbox still
  // owes a wake afterwards (caller should keep polling). Cheap no-op otherwise.
  bool RetryWakeIfPending() const;

  // Test seam: override the event-push function process-wide so a test can force
  // SDL_PushEvent rejection without needing a globally full SDL queue. Passing
  // nullptr restores the default SDL_PushEvent path.
  using EventPusher = std::function<bool(const SDL_Event&)>;
  static void SetEventPusherForTesting(EventPusher pusher);

 private:
  bool PushWakeEvent(Uint32 wake) const;

  std::mutex mutex_;
  std::vector<Action> actions_;
  // Coalescing index for PostLatest: key -> position in actions_. Stable until the
  // next Drain()/Clear() (actions are only replaced in place, never erased
  // mid-stream, so recorded indices never shift). Cleared alongside actions_.
  std::unordered_map<std::string, std::size_t> keyed_index_;
  std::atomic<int> queued_{0};
  std::atomic<Uint32> wake_event_type_{0};
  mutable std::atomic<bool> wake_delivery_failed_{false};
};

}  // namespace microide::util
