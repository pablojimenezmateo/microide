#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
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

  // Wake the UI loop without enqueuing. For nudging the loop to re-poll state
  // that is not delivered as a mailbox action (e.g. a worker-thread exit).
  void PushWake() const;

  // Main thread: run every queued action in FIFO order. Returns the count run.
  int Drain();

  // Drop queued actions without running them (protocol reset / teardown).
  void Clear();

  // Lockless fast-path gate for the scheduled-wake poll: non-zero only while
  // actions await draining.
  int PendingCount() const { return queued_.load(std::memory_order_acquire); }

 private:
  std::mutex mutex_;
  std::vector<Action> actions_;
  std::atomic<int> queued_{0};
  std::atomic<Uint32> wake_event_type_{0};
};

}  // namespace microide::util
