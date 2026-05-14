#include "workspace/WorkspaceProjectFileMonitor.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>
#include <vector>

#include "project/IgnoreMatcher.h"
#include "util/PerformanceTrace.h"

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

    const std::filesystem::path relative = RelativeToRoot(normalized_path);
    if (relative.empty()) {
      return true;
    }

    const auto& matcher = MatcherForParentDirectory(normalized_path.parent_path().lexically_normal());
    if (matcher.Ignored(relative, is_directory)) {
      return false;
    }
    for (std::filesystem::path parent = relative.parent_path();
         !parent.empty() && parent != std::filesystem::path(".");
         parent = parent.parent_path()) {
      if (matcher.Ignored(parent.lexically_normal(), true)) {
        return false;
      }
    }
    return true;
  }

  private:
  std::filesystem::path RelativeToRoot(const std::filesystem::path& path) const {
    const std::filesystem::path relative = path.lexically_relative(root_);
    if (!relative.empty()) {
      return relative.lexically_normal();
    }
#ifdef _WIN32
    const std::string path_text = path.generic_string();
    const std::string root_text = root_.generic_string();
    if (path_text.size() <= root_text.size()) {
      return {};
    }
    auto lower_copy = [](std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return text;
    };
    const std::string lower_path = lower_copy(path_text);
    const std::string lower_root = lower_copy(root_text);
    if (lower_path.rfind(lower_root, 0) != 0 || path_text[root_text.size()] != '/') {
      return {};
    }
    return std::filesystem::path(path_text.substr(root_text.size() + 1)).lexically_normal();
#else
    return {};
#endif
  }

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
  std::scoped_lock lock(state_mutex_);
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
  std::shared_ptr<ProjectTraversalFilter> filter;
  if (project_root.empty()) {
    std::scoped_lock state_lock(state_mutex_);
    ++arm_generation_;
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

  const std::filesystem::path normalized_root = project_root.lexically_normal();
  {
    std::scoped_lock state_lock(state_mutex_);
    ++arm_generation_;
    if (deferred_arming_) {
      pending_project_root_ = normalized_root;
      watched_project_root_.clear();
      deferred_arm_baseline_ = std::filesystem::file_time_type::clock::now();
      traversal_filter_.reset();
      watcher_.SetDeferInitialSnapshot(true);
      watcher_.SetEntryFilter({});
      watcher_.Clear();
      return;
    }

    watched_project_root_ = normalized_root;
    pending_project_root_.clear();
    deferred_arm_baseline_.reset();
    filter = std::make_shared<ProjectTraversalFilter>(normalized_root);
    traversal_filter_ = filter;
  }

  watcher_.SetDeferInitialSnapshot(false);
  watcher_.SetEntryFilter([filter](const std::filesystem::path& path, platform::PathType type) {
    return filter == nullptr || filter->Includes(path, type);
  });
  watcher_.SetRoots({normalized_root});
}

void WorkspaceProjectFileMonitor::ArmPendingWatch() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceProjectFileMonitor::ArmPendingWatch");
  std::filesystem::path root;
  std::uint64_t generation = 0;
  {
    std::scoped_lock state_lock(state_mutex_);
    if (pending_project_root_.empty()) {
      return;
    }
    root = pending_project_root_;
    generation = arm_generation_;
  }

  const auto filter = std::make_shared<ProjectTraversalFilter>(root);
  watcher_.SetDeferInitialSnapshot(true);
  watcher_.SetEntryFilter([filter](const std::filesystem::path& path, platform::PathType type) {
    return filter == nullptr || filter->Includes(path, type);
  });
  watcher_.SetRoots({root});

  std::scoped_lock state_lock(state_mutex_);
  if (generation != arm_generation_ || pending_project_root_ != root) {
    return;
  }
  watched_project_root_ = root;
  pending_project_root_.clear();
  traversal_filter_ = filter;
}

void WorkspaceProjectFileMonitor::Reset() {
  {
    std::scoped_lock state_lock(state_mutex_);
    ++arm_generation_;
    pending_project_root_.clear();
    watched_project_root_.clear();
    deferred_arm_baseline_.reset();
    traversal_filter_.reset();
  }
  watcher_.SetDeferInitialSnapshot(false);
  watcher_.SetEntryFilter({});
  watcher_.Clear();
  std::scoped_lock lock(wake_mutex_);
  wake_event_pending_ = false;
}

std::optional<std::chrono::milliseconds> WorkspaceProjectFileMonitor::NextPollDelay() const {
  {
    std::scoped_lock state_lock(state_mutex_);
    if (!pending_project_root_.empty()) {
      return std::nullopt;
    }
  }
  return watcher_.NextPollDelay();
}

bool WorkspaceProjectFileMonitor::PollForChanges() {
  {
    std::scoped_lock state_lock(state_mutex_);
    if (!pending_project_root_.empty()) {
      return false;
    }
  }
  const std::optional<std::chrono::milliseconds> next_delay = watcher_.NextPollDelay();
  return next_delay.has_value() && *next_delay == std::chrono::milliseconds::zero() &&
         watcher_.Poll();
}

bool WorkspaceProjectFileMonitor::ConsumePendingChanges() {
  EnsureWatching();
  if (HasVisibleChangesSinceDeferredArming()) {
    std::scoped_lock state_lock(state_mutex_);
    deferred_arm_baseline_.reset();
    return true;
  }
  const bool changed = watcher_.Poll();
  if (changed) {
    std::scoped_lock state_lock(state_mutex_);
    deferred_arm_baseline_.reset();
  }
  return changed;
}

bool WorkspaceProjectFileMonitor::EnsureWatching() {
  std::filesystem::path root;
  std::uint64_t generation = 0;
  {
    std::scoped_lock state_lock(state_mutex_);
    if (pending_project_root_.empty()) {
      return false;
    }
    root = pending_project_root_;
    generation = arm_generation_;
  }

  const auto filter = std::make_shared<ProjectTraversalFilter>(root);
  watcher_.SetDeferInitialSnapshot(true);
  watcher_.SetEntryFilter([filter](const std::filesystem::path& path, platform::PathType type) {
    return filter == nullptr || filter->Includes(path, type);
  });
  watcher_.SetRoots({root});

  std::scoped_lock state_lock(state_mutex_);
  if (generation != arm_generation_ || pending_project_root_ != root) {
    return true;
  }
  watched_project_root_ = root;
  pending_project_root_.clear();
  traversal_filter_ = filter;
  return true;
}

bool WorkspaceProjectFileMonitor::HasVisibleChangesSinceDeferredArming() const {
  std::filesystem::path root;
  std::optional<std::filesystem::file_time_type> baseline;
  std::shared_ptr<ProjectTraversalFilter> filter;
  {
    std::scoped_lock state_lock(state_mutex_);
    root = watched_project_root_;
    baseline = deferred_arm_baseline_;
    filter = traversal_filter_;
  }
  if (!baseline.has_value() || filter == nullptr) {
    return false;
  }
  if (root.empty()) {
    return false;
  }

  std::error_code root_error;
  if (!std::filesystem::exists(root, root_error)) {
    return false;
  }

  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  std::error_code iterate_error;
  for (std::filesystem::recursive_directory_iterator it(root, options, iterate_error), end;
       !iterate_error && it != end; it.increment(iterate_error)) {
    const std::filesystem::path path = it->path().lexically_normal();
    const platform::PathType type = platform::ReadPathType(path);
    if (!filter->Includes(path, type)) {
      if (type == platform::PathType::Directory) {
        it.disable_recursion_pending();
      }
      continue;
    }
    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(path, time_error);
    if (!time_error && modified > *baseline) {
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
