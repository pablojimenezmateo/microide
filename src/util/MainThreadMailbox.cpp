#include "util/MainThreadMailbox.h"

#include <utility>

namespace microide::util {

namespace {

// Process-wide event-push override for tests (default: real SDL_PushEvent). A raw
// function object guarded by the caller's single-threaded test setup; production
// leaves it null.
MainThreadMailbox::EventPusher& TestEventPusher() {
  static MainThreadMailbox::EventPusher pusher;
  return pusher;
}

}  // namespace

void MainThreadMailbox::SetEventPusherForTesting(EventPusher pusher) {
  TestEventPusher() = std::move(pusher);
}

void MainThreadMailbox::SetWakeEventType(Uint32 event_type) {
  wake_event_type_.store(event_type, std::memory_order_release);
}

void MainThreadMailbox::Post(Action action) {
  PostWithoutWake(std::move(action));
  PushWake();
}

void MainThreadMailbox::PostWithoutWake(Action action) {
  std::lock_guard lock(mutex_);
  actions_.push_back(std::move(action));
  queued_.store(static_cast<int>(actions_.size()), std::memory_order_release);
}

bool MainThreadMailbox::PushWakeEvent(Uint32 wake) const {
  SDL_Event event{};
  event.type = wake;
  if (const EventPusher& pusher = TestEventPusher()) {
    return pusher(event);
  }
  return SDL_PushEvent(&event);
}

bool MainThreadMailbox::PushWake() const {
  const Uint32 wake = wake_event_type_.load(std::memory_order_acquire);
  if (wake == 0) {
    return false;
  }
  if (PushWakeEvent(wake)) {
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
  std::vector<Action> actions;
  {
    std::lock_guard lock(mutex_);
    actions.swap(actions_);
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
  queued_.store(0, std::memory_order_release);
  wake_delivery_failed_.store(false, std::memory_order_release);
}

}  // namespace microide::util
