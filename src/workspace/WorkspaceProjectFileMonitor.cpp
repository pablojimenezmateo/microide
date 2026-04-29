#include "workspace/WorkspaceProjectFileMonitor.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "project/IgnoreMatcher.h"

namespace microide::workspace {

class WorkspaceProjectFileMonitor::ProjectTraversalFilter {
 public:
  explicit ProjectTraversalFilter(std::filesystem::path root) : root_(std::move(root)) {
    root_matcher_.SetRoot(root_);
  }

  bool Includes(const std::filesystem::path& path, platform::PathType type) {
    const std::filesystem::path normalized_path = path.lexically_normal();
    if (root_.empty() || normalized_path == root_) {
      return true;
    }

    const bool is_directory = type == platform::PathType::Directory;
    if (is_directory && normalized_path.filename() == ".git") {
      return false;
    }

    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(normalized_path, root_, error);
    if (error || relative.empty()) {
      return true;
    }

    const auto& matcher = MatcherForParentDirectory(normalized_path.parent_path().lexically_normal());
    return !matcher.Ignored(relative, is_directory);
  }

 private:
  const project::IgnoreMatcher& MatcherForParentDirectory(const std::filesystem::path& directory) {
    if (directory.empty() || directory == root_) {
      return root_matcher_;
    }

    const std::string key = directory.generic_string();
    const auto existing = directory_matchers_.find(key);
    if (existing != directory_matchers_.end()) {
      return existing->second;
    }

    const auto& parent_matcher = MatcherForParentDirectory(directory.parent_path().lexically_normal());
    project::IgnoreMatcher matcher = parent_matcher;
    matcher.LoadIgnoreFile(directory / ".gitignore");
    return directory_matchers_.emplace(key, std::move(matcher)).first->second;
  }

  std::filesystem::path root_;
  project::IgnoreMatcher root_matcher_;
  std::unordered_map<std::string, project::IgnoreMatcher> directory_matchers_;
};

WorkspaceProjectFileMonitor::WorkspaceProjectFileMonitor() = default;

WorkspaceProjectFileMonitor::~WorkspaceProjectFileMonitor() = default;

void WorkspaceProjectFileMonitor::SetDeferredArming(bool deferred) {
  deferred_arming_ = deferred;
}

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
    pending_project_root_.clear();
    watched_project_root_.clear();
    deferred_arm_baseline_.reset();
    traversal_filter_.reset();
    watcher_.SetDeferInitialSnapshot(false);
    watcher_.SetEntryFilter({});
    watcher_.Clear();
    std::scoped_lock lock(wake_mutex_);
    wake_event_pending_ = false;
    return;
  }

  if (!deferred_arming_) {
    watched_project_root_ = project_root.lexically_normal();
    traversal_filter_ =
        std::make_unique<ProjectTraversalFilter>(project_root.lexically_normal());
    watcher_.SetDeferInitialSnapshot(false);
    watcher_.SetEntryFilter([this](const std::filesystem::path& path, platform::PathType type) {
      return traversal_filter_ == nullptr || traversal_filter_->Includes(path, type);
    });
    watcher_.SetRoots({project_root.lexically_normal()});
    pending_project_root_.clear();
    deferred_arm_baseline_.reset();
    return;
  }

  pending_project_root_ = project_root.lexically_normal();
  watched_project_root_.clear();
  deferred_arm_baseline_ = std::filesystem::file_time_type::clock::now();
  traversal_filter_.reset();
  watcher_.SetDeferInitialSnapshot(true);
  watcher_.SetEntryFilter({});
  watcher_.Clear();
}

void WorkspaceProjectFileMonitor::Reset() {
  pending_project_root_.clear();
  watched_project_root_.clear();
  deferred_arm_baseline_.reset();
  traversal_filter_.reset();
  watcher_.SetDeferInitialSnapshot(false);
  watcher_.SetEntryFilter({});
  watcher_.Clear();
  std::scoped_lock lock(wake_mutex_);
  wake_event_pending_ = false;
}

std::optional<std::chrono::milliseconds> WorkspaceProjectFileMonitor::NextPollDelay() const {
  if (!pending_project_root_.empty()) {
    return std::chrono::milliseconds(1);
  }
  return watcher_.NextPollDelay();
}

bool WorkspaceProjectFileMonitor::PollForChanges() {
  if (EnsureWatching()) {
    return false;
  }
  const std::optional<std::chrono::milliseconds> next_delay = watcher_.NextPollDelay();
  return next_delay.has_value() && *next_delay == std::chrono::milliseconds::zero() &&
         watcher_.Poll();
}

bool WorkspaceProjectFileMonitor::ConsumePendingChanges() {
  if (EnsureWatching()) {
    if (HasVisibleChangesSinceDeferredArming()) {
      deferred_arm_baseline_.reset();
      return true;
    }
    deferred_arm_baseline_.reset();
  }
  return watcher_.Poll();
}

bool WorkspaceProjectFileMonitor::EnsureWatching() {
  if (pending_project_root_.empty()) {
    return false;
  }

  traversal_filter_ =
      std::make_unique<ProjectTraversalFilter>(pending_project_root_);
  watched_project_root_ = pending_project_root_;
  watcher_.SetDeferInitialSnapshot(true);
  watcher_.SetEntryFilter([this](const std::filesystem::path& path, platform::PathType type) {
    return traversal_filter_ == nullptr || traversal_filter_->Includes(path, type);
  });
  watcher_.SetRoots({pending_project_root_});
  pending_project_root_.clear();
  return true;
}

bool WorkspaceProjectFileMonitor::HasVisibleChangesSinceDeferredArming() const {
  if (!deferred_arm_baseline_.has_value() || traversal_filter_ == nullptr) {
    return false;
  }

  const std::filesystem::path root = watched_project_root_;
  if (root.empty()) {
    return false;
  }

  std::error_code root_error;
  if (!std::filesystem::exists(root, root_error)) {
    return false;
  }

  std::error_code root_time_error;
  const auto root_time = std::filesystem::last_write_time(root, root_time_error);
  if (!root_time_error &&
      traversal_filter_->Includes(root, platform::ReadPathType(root)) &&
      root_time > *deferred_arm_baseline_) {
    return true;
  }

  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  std::error_code iterate_error;
  for (std::filesystem::recursive_directory_iterator it(root, options, iterate_error), end;
       !iterate_error && it != end; it.increment(iterate_error)) {
    const std::filesystem::path path = it->path().lexically_normal();
    const platform::PathType type = platform::ReadPathType(path);
    if (!traversal_filter_->Includes(path, type)) {
      if (type == platform::PathType::Directory) {
        it.disable_recursion_pending();
      }
      continue;
    }
    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(path, time_error);
    if (!time_error && modified > *deferred_arm_baseline_) {
      return true;
    }
  }

  return false;
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
