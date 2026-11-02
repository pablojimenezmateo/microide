#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace microide::workspace {

// When the crash-safety session flush (WorkspaceShell::FlushSessionStateForCrashSafety)
// stages the unsaved buffers to disk: once the edits have been quiet for kQuietMs, so
// active typing coalesces into one write -- but never later than kMaxWaitMs after the
// first edit it has not staged yet. A pure debounce, which this was, let steady typing
// (every key within the quiet window of the last) push the deadline back forever: a
// crash after ten minutes of it lost all ten. Pure and clock-free (the caller passes
// SDL_GetTicks()), so the rule is tested without a window.
struct CrashSafetyFlushSchedule {
  static constexpr std::uint64_t kQuietMs = 2500;
  static constexpr std::uint64_t kMaxWaitMs = 10000;

  // Something unsaved changed at `now_ms`: a buffer edit, a host terminal that got
  // its handle. Arms the schedule, keeping the first unstaged change's time.
  void Note(std::uint64_t now_ms) {
    if (!armed_) {
      armed_ = true;
      first_ms_ = now_ms;
    }
    last_ms_ = now_ms;
  }

  // An edit that leaves nothing new to stage: the quiet window restarts only if a
  // flush is already owed (re-arming is Note's call).
  void Touch(std::uint64_t now_ms) {
    if (armed_) {
      last_ms_ = now_ms;
    }
  }

  bool armed() const { return armed_; }

  // Due at `now_ms`: the caller flushes and calls Disarm.
  bool Due(std::uint64_t now_ms) const { return armed_ && now_ms >= Deadline(); }

  // How long the idle loop may sleep before the flush is due; nullopt when nothing
  // is owed, 0 when it is due already.
  std::optional<std::uint64_t> DelayMs(std::uint64_t now_ms) const {
    if (!armed_) {
      return std::nullopt;
    }
    const std::uint64_t deadline = Deadline();
    return now_ms >= deadline ? 0 : deadline - now_ms;
  }

  void Disarm() { armed_ = false; }

 private:
  std::uint64_t Deadline() const { return std::min(last_ms_ + kQuietMs, first_ms_ + kMaxWaitMs); }

  std::uint64_t first_ms_ = 0;
  std::uint64_t last_ms_ = 0;
  bool armed_ = false;
};

}  // namespace microide::workspace
