#pragma once

#include <SDL3/SDL.h>

#include <algorithm>

#include "workspace/WorkspaceShell.h"

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
