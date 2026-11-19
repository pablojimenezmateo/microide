#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::app {

// How the event loop should wait for the next SDL event this iteration.
enum class IdleWaitMode {
  Poll,         // non-blocking SDL_PollEvent (a redraw/animation is pending)
  WaitTimeout,  // SDL_WaitEventTimeout up to `timeout_ms` (caret blink pending)
  Wait,         // blocking SDL_WaitEvent (fully idle)
};

struct IdleWaitDecision {
  IdleWaitMode mode = IdleWaitMode::Wait;
  Sint32 timeout_ms = 0;  // only meaningful for WaitTimeout
};

// The deadline by which the loop owes the scheduled wake after a wait: the loop runs
// the wake (WorkspaceShell::HandleScheduledWake -- the crash-safety flush, a toast's
// expiry, search-as-you-type) when a wait ends with no event, so a stream of events
// -- a large file's highlight prefetch posts one after every frame -- ended every
// timed wait before its timeout and starved it: the flush owed within 10 s of an
// edit did not run while the stream lasted (soak 5fae008f5eb2). A timed wait an event
// ends keeps its deadline (the earliest one owed stays); a wait with no event ran the
// wake and owes nothing.
inline std::optional<Uint64> OwedWakeDeadline(std::optional<Uint64> owed,
                                              const IdleWaitDecision& wait,
                                              Uint64 wait_started_ms,
                                              bool has_event) {
  if (!has_event) {
    return std::nullopt;
  }
  if (wait.mode != IdleWaitMode::WaitTimeout) {
    return owed;
  }
  const Uint64 deadline = wait_started_ms + static_cast<Uint64>(std::max<Sint32>(0, wait.timeout_ms));
  return owed.has_value() ? std::min(*owed, deadline) : deadline;
}

// Map an IdleWaitState hint onto the concrete wait strategy. Pure so the policy
// can be unit-tested without driving the SDL event loop. The CaretOnly timeout
// is clamped to at least 1ms so SDL_WaitEventTimeout never busy-spins.
inline IdleWaitDecision ChooseIdleWait(const workspace::WorkspaceShell::IdleWaitState& state) {
  switch (state.hint) {
    case workspace::WorkspaceShell::IdleHint::Full:
      return IdleWaitDecision{.mode = IdleWaitMode::Poll, .timeout_ms = 0};
    case workspace::WorkspaceShell::IdleHint::CaretOnly:
      return IdleWaitDecision{
          .mode = IdleWaitMode::WaitTimeout,
          .timeout_ms = static_cast<Sint32>(std::max<Uint32>(1, state.caret_remaining_ms)),
      };
    case workspace::WorkspaceShell::IdleHint::Idle:
      break;
  }
  return IdleWaitDecision{.mode = IdleWaitMode::Wait, .timeout_ms = 0};
}

}  // namespace microide::app
