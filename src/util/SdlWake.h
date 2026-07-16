#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <utility>

// Shared, checked SDL wake-event pusher for one-shot producers (git blame, control
// channel, native dialogs, highlight prefetch, background-task idle wakes). Each of
// these makes its result ready in shared state and then pushes a neutral custom SDL
// event so the main loop wakes and consumes it. A bare SDL_PushEvent() is treated as
// fire-and-forget: if the event queue rejects the push, the ready data is never
// drained until some unrelated event happens to wake a blocked SDL_WaitEventTimeout.
//
// PushSdlWake reports whether the event was queued AND latches a process-wide
// "wake owed" bit on failure. CurrentIdleWaitState() consumes that bit to schedule a
// short fallback wait, so a dropped wake self-heals within one poll interval instead
// of stranding the producer's ready state. (TD-2026-07-16-54.)
namespace microide::util {

using SdlEventPusher = std::function<bool(const SDL_Event&)>;

// Test seam: override the actual push (default SDL_PushEvent) so a test can force a
// rejected push without a globally full SDL queue. One instance across all TUs.
inline SdlEventPusher& SdlEventPusherHook() {
  static SdlEventPusher hook;
  return hook;
}

inline void SetSdlEventPusherForTesting(SdlEventPusher pusher) {
  SdlEventPusherHook() = std::move(pusher);
}

// Process-wide "a wake push failed and its producer's ready state still needs
// draining" flag. Set by PushSdlWake on failure; consumed by the idle-wait poll.
inline std::atomic<bool>& SdlWakeOwedFlag() {
  static std::atomic<bool> owed{false};
  return owed;
}

// True while at least one wake push has failed since the last consume. Non-mutating
// peek for tests / diagnostics.
inline bool HasOwedSdlWake() { return SdlWakeOwedFlag().load(std::memory_order_acquire); }

// Test-and-clear: returns whether a wake was owed and clears the flag. The idle-wait
// computation calls this to decide whether to shorten its blocking timeout; if a
// producer's push keeps failing it re-latches on the next PushSdlWake.
inline bool ConsumeOwedSdlWake() {
  return SdlWakeOwedFlag().exchange(false, std::memory_order_acq_rel);
}

// Process-wide "at least one custom wake-event registration failed at startup" flag.
// When set, some subsystem's wake channel could not be allocated (SDL_RegisterEvents
// returned -1), so its ready state has no event to drain it. The idle-wait poll consults
// this to fall back to a bounded wait instead of blocking forever. (TD-2026-07-16-56.)
inline std::atomic<bool>& SdlWakeRegistrationDegradedFlag() {
  static std::atomic<bool> degraded{false};
  return degraded;
}

inline void SetSdlWakeRegistrationDegraded(bool degraded) {
  SdlWakeRegistrationDegradedFlag().store(degraded, std::memory_order_release);
}

inline bool SdlWakeRegistrationDegraded() {
  return SdlWakeRegistrationDegradedFlag().load(std::memory_order_acquire);
}

// Push a neutral wake event of `type`. Returns whether it was queued. `type == 0`
// (wake disabled) is a no-op returning false without latching the owed bit. A
// rejected push latches the owed bit so the idle poll schedules a fallback wait.
inline bool PushSdlWake(Uint32 type) {
  if (type == 0) {
    return false;
  }
  SDL_Event event{};
  event.type = type;
  const SdlEventPusher& hook = SdlEventPusherHook();
  const bool queued = hook ? hook(event) : SDL_PushEvent(&event);
  if (!queued) {
    SdlWakeOwedFlag().store(true, std::memory_order_release);
  }
  return queued;
}

}  // namespace microide::util
