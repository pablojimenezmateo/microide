#pragma once

#include <cstdint>

#include "util/Waker.h"

namespace microide::workspace {

// Paces the automatic git refreshes -- a file-watcher change, a save, a terminal
// command, a commit waiting on a fresh snapshot -- to one at a time and at least
// kIntervalMs apart, WITHOUT dropping any. A request turned away (one is running,
// or the last started less than kIntervalMs ago) is kept and issued when it is
// allowed: when the running refresh's snapshot is consumed, or when the interval
// ends, for which it wakes the event loop on `wake_channel` (an SDL timer).
//
// It used to drop them. A change inside the interval only marked the snapshot
// stale, and nothing asked again, so the git view kept showing the old state
// until some unrelated refresh -- and a commit waiting on that refresh waited
// forever (QA journey-feature-branch on CI: `git reset` of a staged file, then
// Commit 0.6 s after the last refresh).
//
// One at a time because a refresh requested while one runs supersedes it (its
// result is discarded): under a stream of watcher changes, requesting each one
// would discard every result and publish none.
class AutomaticGitRefreshPacer {
 public:
  static constexpr std::uint64_t kIntervalMs = 750;

  // `wake_channel` is read when a wake is armed, so it may be registered later;
  // 0 arms nothing (tests drive Resume() themselves).
  explicit AutomaticGitRefreshPacer(const util::WakeChannel& wake_channel)
      : wake_channel_(wake_channel) {}

  // A refresh is wanted. True when the caller should issue it now; otherwise it
  // is kept for Resume().
  [[nodiscard]] bool Request(std::uint64_t now_ms, bool refresh_running);

  // On the main thread when a refresh's snapshot was consumed or the armed wake
  // fired: true when a kept request is due now, for the caller to Request() and
  // issue; otherwise it stays kept, its wake armed.
  [[nodiscard]] bool Resume(std::uint64_t now_ms, bool refresh_running);

  [[nodiscard]] bool pending() const { return pending_; }
  // The deadline a wake is armed for; 0 when none.
  [[nodiscard]] std::uint64_t wake_armed_for_ms() const { return wake_armed_for_ms_; }

 private:
  const util::WakeChannel& wake_channel_;
  bool pending_ = false;
  std::uint64_t next_allowed_ms_ = 0;
  std::uint64_t wake_armed_for_ms_ = 0;
};

}  // namespace microide::workspace
