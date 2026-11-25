#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::app {

enum class IdleWaitMode {
  Poll,
  WaitTimeout,
  Wait,
};

struct IdleWaitDecision {
  IdleWaitMode mode = IdleWaitMode::Wait;
  Sint32 timeout_ms = 0;
};

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

}
