#include "util/MainThreadMailbox.h"

#include <utility>

namespace microide::util {

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

void MainThreadMailbox::PushWake() const {
  const Uint32 wake = wake_event_type_.load(std::memory_order_acquire);
  if (wake != 0) {
    SDL_Event event{};
    event.type = wake;
    SDL_PushEvent(&event);
  }
}

int MainThreadMailbox::Drain() {
  std::vector<Action> actions;
  {
    std::lock_guard lock(mutex_);
    actions.swap(actions_);
    queued_.store(0, std::memory_order_release);
  }
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
}

}  // namespace microide::util
