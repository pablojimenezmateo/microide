#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

// Shared, checked wake-the-UI-loop pusher for one-shot producers (git blame, control
// channel, native dialogs, highlight prefetch, background-task idle wakes). Each of
// these makes its result ready in shared state and then wakes the loop so it drains
// the result. A fire-and-forget wake is not enough: if the loop's queue rejects the
// wake, the ready data is never drained until some unrelated event happens to wake a
// blocked loop.
//
// PushWake reports whether the wake was delivered AND latches a process-wide
// "wake owed" bit on failure. CurrentIdleWaitState() consumes that bit to schedule a
// short fallback wait, so a dropped wake self-heals within one poll interval instead
// of stranding the producer's ready state. (TD-2026-07-16-54.)
//
// **This header names no windowing library on purpose.** A `WakeChannel` is an opaque
// id whose meaning belongs to whoever installed the pusher: the desktop shell installs
// an SDL binding (`app::InstallSdlWaker`) where a channel is an `SDL_RegisterEvents`
// custom event type; a headless host installs its own. That keeps every producer —
// terminal, git, search, control channel, plugin worker — free of the windowing
// library, which is what lets them run in a process that has no window at all.
namespace microide::util {

// An opaque wake-channel id. 0 means "waking disabled": producers still make their
// result ready, and the loop's scheduled poll is what picks it up.
using WakeChannel = std::uint32_t;

// Delivers a wake on `channel`. Returns whether the loop accepted it.
using WakePusher = std::function<bool(WakeChannel)>;

// The host binding, installed once at startup. Unset means every push fails (and so
// latches the owed bit), which is the correct degraded behaviour for a process with
// no event loop: nothing is silently dropped.
inline WakePusher& InstalledWakePusher() {
  static WakePusher pusher;
  return pusher;
}

inline void SetWakePusher(WakePusher pusher) { InstalledWakePusher() = std::move(pusher); }

// Test seam: override the installed pusher so a test can force a rejected wake
// without needing a globally full event queue. Passing nullptr restores the installed
// host pusher (it does NOT clear it). One instance across all TUs.
inline WakePusher& WakePusherOverride() {
  static WakePusher pusher;
  return pusher;
}

inline void SetWakePusherForTesting(WakePusher pusher) {
  WakePusherOverride() = std::move(pusher);
}

// Process-wide "a wake push failed and its producer's ready state still needs
// draining" flag. Set by PushWake on failure; consumed by the idle-wait poll.
inline std::atomic<bool>& WakeOwedFlag() {
  static std::atomic<bool> owed{false};
  return owed;
}

// True while at least one wake push has failed since the last consume. Non-mutating
// peek for tests / diagnostics.
inline bool HasOwedWake() { return WakeOwedFlag().load(std::memory_order_acquire); }

// Test-and-clear: returns whether a wake was owed and clears the flag. The idle-wait
// computation calls this to decide whether to shorten its blocking timeout; if a
// producer's push keeps failing it re-latches on the next PushWake.
inline bool ConsumeOwedWake() {
  return WakeOwedFlag().exchange(false, std::memory_order_acq_rel);
}

// Process-wide "at least one wake-channel registration failed at startup" flag.
// When set, some subsystem's wake channel could not be allocated, so its ready state
// has no wake to drain it. The idle-wait poll consults this to fall back to a bounded
// wait instead of blocking forever. (TD-2026-07-16-56.)
inline std::atomic<bool>& WakeRegistrationDegradedFlag() {
  static std::atomic<bool> degraded{false};
  return degraded;
}

inline void SetWakeRegistrationDegraded(bool degraded) {
  WakeRegistrationDegradedFlag().store(degraded, std::memory_order_release);
}

inline bool WakeRegistrationDegraded() {
  return WakeRegistrationDegradedFlag().load(std::memory_order_acquire);
}

// Deliver a wake on `channel` WITHOUT latching the shared owed bit. For callers that
// track their own undelivered-wake state and retry it themselves (MainThreadMailbox).
// `channel == 0` (wake disabled) is a no-op returning false.
inline bool DeliverWake(WakeChannel channel) {
  if (channel == 0) {
    return false;
  }
  if (const WakePusher& override_pusher = WakePusherOverride()) {
    return override_pusher(channel);
  }
  const WakePusher& pusher = InstalledWakePusher();
  return pusher ? pusher(channel) : false;
}

// Push a wake on `channel`. Returns whether it was delivered. `channel == 0` (wake
// disabled) is a no-op returning false without latching the owed bit. A rejected push
// latches the owed bit so the idle poll schedules a fallback wait.
inline bool PushWake(WakeChannel channel) {
  if (channel == 0) {
    return false;
  }
  const bool delivered = DeliverWake(channel);
  if (!delivered) {
    WakeOwedFlag().store(true, std::memory_order_release);
  }
  return delivered;
}

}  // namespace microide::util
