#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace microide::app {

// Re-delivers a cross-thread wake that SDL lost (TD-2026-11-07-433).
//
// SDL 3.4's SDL_WaitEventTimeout_Device checks the event queue, THEN publishes the
// window its backend may wake (`wakeup_window`, an atomic -- the lock older SDL held
// across both is gone), then blocks; there is no re-check in between. An
// SDL_PushEvent from another thread that lands in that window enqueues its event
// and finds no window to wake, so the main thread blocks with the event already
// queued -- for good, when its wait has no timeout. Seen as the control channel
// going silent on Wayland under load: the main thread in poll(display fd, -1), the
// control thread alive, the command queued.
//
// The relay closes it without costing an idle wake. The main loop brackets each
// blocking wait with EnterBlockingWait/LeaveBlockingWait; every pushed wake calls
// NotePush. A push that lands while the main loop is blocked arms the relay, which
// looks again `delay` later: if the loop is STILL blocked with a push newer than
// its wait, that push's wake was lost (or is unusually slow) and the relay calls
// `nudge` -- one more pushed event, whose wake does go through, because by then the
// waiting thread has published its window. A spurious nudge is one no-op event.
// With nothing pushed, the relay thread sleeps on its condition variable.
class LostWakeRelay {
 public:
  using Nudge = std::function<void()>;

  LostWakeRelay(std::chrono::milliseconds delay, Nudge nudge);
  ~LostWakeRelay();

  LostWakeRelay(const LostWakeRelay&) = delete;
  LostWakeRelay& operator=(const LostWakeRelay&) = delete;

  // Any thread, AFTER the wake's event is enqueued.
  void NotePush();
  // The main loop, immediately before and after a blocking SDL wait.
  void EnterBlockingWait();
  void LeaveBlockingWait();

  // Diagnostics and tests: how many nudges the relay has sent.
  std::uint64_t nudges() const { return nudges_.load(std::memory_order_relaxed); }

 private:
  void Run();
  bool WakeLooksLost() const;

  const std::chrono::milliseconds delay_;
  const Nudge nudge_;
  std::atomic<std::uint64_t> pushes_{0};
  std::atomic<std::uint64_t> pushes_at_wait_entry_{0};
  std::atomic<bool> in_wait_{false};
  std::atomic<std::uint64_t> nudges_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool armed_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace microide::app
