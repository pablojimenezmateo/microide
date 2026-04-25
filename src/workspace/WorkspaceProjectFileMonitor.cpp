#include "workspace/WorkspaceProjectFileMonitor.h"

#include <utility>
#include <vector>

namespace microide::workspace {

void WorkspaceProjectFileMonitor::SetPollInterval(std::chrono::milliseconds poll_interval) {
  watcher_.SetPollInterval(poll_interval);
}

void WorkspaceProjectFileMonitor::SetWakeEventType(Uint32 event_type) {
  {
    std::scoped_lock lock(wake_mutex_);
    wake_event_type_ = event_type;
    wake_event_pending_ = false;
  }

  if (event_type == 0) {
    watcher_.SetWakeCallback({});
    return;
  }
  watcher_.SetWakeCallback([this]() { PushWakeEvent(); });
}

bool WorkspaceProjectFileMonitor::ConsumeWakeEvent(Uint32 type) {
  std::scoped_lock lock(wake_mutex_);
  if (wake_event_type_ == 0 || type != wake_event_type_) {
    return false;
  }
  wake_event_pending_ = false;
  return true;
}

void WorkspaceProjectFileMonitor::SetProjectRoot(const std::filesystem::path& project_root) {
  if (project_root.empty()) {
    watcher_.Clear();
    std::scoped_lock lock(wake_mutex_);
    wake_event_pending_ = false;
    return;
  }

  watcher_.SetRoots({project_root.lexically_normal()});
}

void WorkspaceProjectFileMonitor::Reset() {
  watcher_.Clear();
  std::scoped_lock lock(wake_mutex_);
  wake_event_pending_ = false;
}

std::optional<std::chrono::milliseconds> WorkspaceProjectFileMonitor::NextPollDelay() const {
  return watcher_.NextPollDelay();
}

bool WorkspaceProjectFileMonitor::PollForChanges() {
  const std::optional<std::chrono::milliseconds> next_delay = watcher_.NextPollDelay();
  return next_delay.has_value() && *next_delay == std::chrono::milliseconds::zero() &&
         watcher_.Poll();
}

bool WorkspaceProjectFileMonitor::ConsumePendingChanges() {
  return watcher_.Poll();
}

bool WorkspaceProjectFileMonitor::ReserveWakeEvent(Uint32* event_type) const {
  if (event_type == nullptr) {
    return false;
  }

  std::scoped_lock lock(wake_mutex_);
  if (wake_event_type_ == 0 || wake_event_pending_) {
    return false;
  }

  wake_event_pending_ = true;
  *event_type = wake_event_type_;
  return true;
}

void WorkspaceProjectFileMonitor::PushWakeEvent() const {
  Uint32 event_type = 0;
  if (!ReserveWakeEvent(&event_type)) {
    return;
  }

  SDL_Event event{};
  event.type = event_type;
  if (!SDL_PushEvent(&event)) {
    std::scoped_lock lock(wake_mutex_);
    wake_event_pending_ = false;
  }
}

}  // namespace microide::workspace
