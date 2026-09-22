#include "util/MainThreadMailbox.h"

#include <utility>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microide::util {

void MainThreadMailbox::SetWakeChannel(WakeChannel channel) {
  wake_channel_.store(channel, std::memory_order_release);
}

void MainThreadMailbox::Post(Action action) {
  PostWithoutWake(std::move(action));
  PushWake();
}

void MainThreadMailbox::PostWithoutWake(Action action) {
  AddPerformanceCounter(PerfCounterId::MainThreadMailboxPosts);
  std::lock_guard lock(mutex_);
  actions_.push_back(std::move(action));
  queued_.store(static_cast<int>(actions_.size()), std::memory_order_release);
}

void MainThreadMailbox::PostLatest(std::string key, Action action) {
  // Counted here too, not only in PostWithoutWake: PostLatest has its own body
  // and does not route through it, so a service that posts exclusively through
  // the coalescing path would read as an idle mailbox.
  AddPerformanceCounter(PerfCounterId::MainThreadMailboxPosts);
  {
    std::lock_guard lock(mutex_);
    if (const auto it = keyed_index_.find(key);
        it != keyed_index_.end() && it->second < actions_.size()) {
      // Replace the superseded closure in place (its captured payload is dropped).
      AddPerformanceCounter(PerfCounterId::MainThreadMailboxPostsCoalesced);
      actions_[it->second] = std::move(action);
    } else {
      keyed_index_[std::move(key)] = actions_.size();
      actions_.push_back(std::move(action));
      queued_.store(static_cast<int>(actions_.size()), std::memory_order_release);
    }
  }
  PushWake();
}

bool MainThreadMailbox::PushWake() const {
  const WakeChannel wake = wake_channel_.load(std::memory_order_acquire);
  if (wake == 0) {
    return false;
  }
  // DeliverWake, not PushWake: the mailbox latches and retries its OWN undelivered
  // wake below (RetryWakeIfPending), so it does not also need the shared owed bit.
  if (DeliverWake(wake)) {
    // A delivered wake supersedes any earlier failure: the loop will drain.
    wake_delivery_failed_.store(false, std::memory_order_release);
    return true;
  }
  // Push rejected (full queue). Keep the queued actions and latch the undelivered
  // wake so the scheduled poll retries — silently losing the only wake would strand
  // LSP replies / plugin completions / commit results until unrelated input.
  if (PendingCount() > 0) {
    wake_delivery_failed_.store(true, std::memory_order_release);
  }
  return false;
}

bool MainThreadMailbox::RetryWakeIfPending() const {
  if (PendingCount() == 0) {
    wake_delivery_failed_.store(false, std::memory_order_release);
    return false;
  }
  if (!wake_delivery_failed_.load(std::memory_order_acquire)) {
    return false;  // nothing owed
  }
  PushWake();  // clears the bit on success, re-latches on repeat failure
  return wake_delivery_failed_.load(std::memory_order_acquire);
}

int MainThreadMailbox::Drain() {
  // Runs on the shell thread inside the event loop, so a background service that
  // posts per-item instead of per-batch turns into main-thread cost. The
  // posts/drains ratio is the tell.
  PerformanceTrace::Scope perf_scope("util::MainThreadMailbox::Drain");
  AddPerformanceCounter(PerfCounterId::MainThreadMailboxDrains);
  std::vector<Action> actions;
  {
    std::lock_guard lock(mutex_);
    actions.swap(actions_);
    keyed_index_.clear();
    queued_.store(0, std::memory_order_release);
  }
  // The drain satisfies whatever wake was owed.
  wake_delivery_failed_.store(false, std::memory_order_release);
  for (auto& action : actions) {
    if (action) {
      action();
    }
  }
  return static_cast<int>(actions.size());
}

void MainThreadMailbox::Clear() {
  std::lock_guard lock(mutex_);
  actions_.clear();
  keyed_index_.clear();
  queued_.store(0, std::memory_order_release);
  wake_delivery_failed_.store(false, std::memory_order_release);
}

}  // namespace microide::util
