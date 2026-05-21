#include "workspace/WorkspacePluginAssetMonitor.h"

#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"

namespace microide::workspace {

namespace {

std::filesystem::path GlobalPluginDirectory() {
  const std::filesystem::path app_directory =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return app_directory.empty() ? std::filesystem::path{} : app_directory / "plugins";
}

}  // namespace

void WorkspacePluginAssetMonitor::SetPollInterval(std::chrono::milliseconds poll_interval) {
  watcher_.SetPollInterval(poll_interval);
}

void WorkspacePluginAssetMonitor::SetWakeEventType(Uint32 event_type) {
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

bool WorkspacePluginAssetMonitor::ConsumeWakeEvent(Uint32 type) {
  std::scoped_lock lock(wake_mutex_);
  if (wake_event_type_ == 0 || type != wake_event_type_) {
    return false;
  }
  wake_event_pending_ = false;
  return true;
}

void WorkspacePluginAssetMonitor::SetProjectRoot(const std::filesystem::path& /*project_root*/) {
  std::vector<std::filesystem::path> roots;
  roots.push_back(GlobalPluginDirectory());
  watcher_.SetRoots(std::move(roots));
}

void WorkspacePluginAssetMonitor::Reset() {
  watcher_.Clear();
  std::scoped_lock lock(wake_mutex_);
  wake_event_pending_ = false;
}

std::optional<std::chrono::milliseconds> WorkspacePluginAssetMonitor::NextPollDelay() const {
  return watcher_.NextPollDelay();
}

bool WorkspacePluginAssetMonitor::PollForChanges() {
  const std::optional<std::chrono::milliseconds> next_delay = watcher_.NextPollDelay();
  return next_delay.has_value() && *next_delay == std::chrono::milliseconds::zero() &&
         watcher_.Poll();
}

bool WorkspacePluginAssetMonitor::ConsumePendingChanges() {
  return watcher_.Poll();
}

bool WorkspacePluginAssetMonitor::ReserveWakeEvent(Uint32* event_type) const {
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

void WorkspacePluginAssetMonitor::PushWakeEvent() const {
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
