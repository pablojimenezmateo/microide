#include "workspace/WorkspacePluginAssetMonitor.h"

#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"

namespace microide::workspace {

namespace {

std::filesystem::path GlobalPluginDirectory() {
  const std::filesystem::path app_directory =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return app_directory.empty() ? std::filesystem::path{} : app_directory / "plugins";
}

#ifndef MICROIDE_TESTING
std::filesystem::path RepoPluginDirectory() {
  const auto repo_plugins_from_root = [](const std::filesystem::path& start) {
    if (start.empty()) {
      return std::filesystem::path{};
    }
    std::error_code error;
    std::filesystem::path current = std::filesystem::weakly_canonical(start, error);
    if (error) {
      current = start.lexically_normal();
    }
    while (!current.empty()) {
      const std::filesystem::path plugins_dir = current / "plugins";
      if (platform::ReadPathType(plugins_dir) == platform::PathType::Directory &&
          platform::ReadPathType(plugins_dir / "README.md") == platform::PathType::RegularFile) {
        return plugins_dir.lexically_normal();
      }
      const std::filesystem::path parent = current.parent_path();
      if (parent == current) {
        break;
      }
      current = parent;
    }
    return std::filesystem::path{};
  };

  if (const char* raw_base_path = SDL_GetBasePath();
      raw_base_path != nullptr && raw_base_path[0] != '\0') {
    if (const std::filesystem::path plugins_dir =
            repo_plugins_from_root(std::filesystem::path(raw_base_path));
        !plugins_dir.empty()) {
      return plugins_dir;
    }
  }

  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  return error ? std::filesystem::path{} : repo_plugins_from_root(cwd);
}
#endif

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

void WorkspacePluginAssetMonitor::SetProjectRoot(const std::filesystem::path& project_root) {
  std::vector<std::filesystem::path> roots;
  roots.push_back(GlobalPluginDirectory());
#ifndef MICROIDE_TESTING
  if (const std::filesystem::path repo_plugins = RepoPluginDirectory(); !repo_plugins.empty()) {
    roots.push_back(std::move(repo_plugins));
  }
#endif
  if (!project_root.empty()) {
    roots.push_back(project_root.lexically_normal() / ".microide" / "plugins");
  }
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
