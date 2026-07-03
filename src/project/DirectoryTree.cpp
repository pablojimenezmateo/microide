#include "project/DirectoryTree.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "platform/Filesystem.h"
#include "project/GitStatusService.h"
#include "project/IgnoreMatcher.h"
#include "project/SymlinkLoopGuard.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

struct SortableEntry {
  std::filesystem::path path;
  std::string sort_key;
  bool is_directory = false;
  bool is_symlink = false;
  bool ignored = false;
  IgnoreMatcher matcher;
};

std::string DisplayName(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  if (!name.empty()) {
    return name;
  }
  return path.string();
}

}  // namespace

bool DirectoryTree::SetRoot(const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("DirectoryTree::SetRoot");
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }

  root_ = absolute_root;
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  manually_collapsed_paths_.clear();
  RebuildEntries(false);
  return true;
}

void DirectoryTree::Refresh() {
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::Refresh");
  if (root_.empty()) {
    return;
  }
  RebuildEntries(false);
}

void DirectoryTree::RefreshGitStatuses() {
  if (root_.empty()) {
    return;
  }
  util::StartupTrace::Scope trace_scope("DirectoryTree::RefreshGitStatuses");
  ApplyGitStatuses(CollectGitStatuses(root_));
}

void DirectoryTree::ApplyGitStatuses(std::unordered_map<std::string, GitFileStatus> statuses) {
  git_statuses_ = std::move(statuses);
  for (auto& entry : entries_) {
    entry.git_status = EntryGitStatus(entry.path);
  }
}

void DirectoryTree::MoveSelection(int delta) {
  if (entries_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(selected_index_);
  const int max_index = static_cast<int>(entries_.size()) - 1;
  selected_index_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

void DirectoryTree::SetSelectedIndex(std::size_t index) {
  if (entries_.empty()) {
    selected_index_ = 0;
    return;
  }
  selected_index_ = std::min(index, entries_.size() - 1);
}

bool DirectoryTree::SelectPath(const std::filesystem::path& path) {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }

  const auto normalized_path = absolute_path.lexically_normal();
  if (normalized_path == root_) {
    selected_index_ = 0;
    return true;
  }

  const auto relative = normalized_path.lexically_relative(root_);
  if (relative.empty() ||
      (relative.begin() != relative.end() &&
       *relative.begin() == std::filesystem::path(".."))) {
    return false;
  }

  for (auto current = normalized_path.parent_path(); !current.empty() && current != root_;
       current = current.parent_path()) {
    const auto current_relative = current.lexically_relative(root_);
    if (current_relative.empty() ||
        (current_relative.begin() != current_relative.end() &&
         *current_relative.begin() == std::filesystem::path(".."))) {
      break;
    }
    expanded_paths_.insert(NormalizePathKey(current));
    manually_collapsed_paths_.erase(NormalizePathKey(current));
  }

  RebuildEntries(false);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized_path) {
      selected_index_ = i;
      return true;
    }
  }
  return false;
}

bool DirectoryTree::SelectPathIfVisible(const std::filesystem::path& path) {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }
  const auto normalized_path = absolute_path.lexically_normal();
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized_path) {
      selected_index_ = i;
      return true;
    }
  }
  return false;
}

bool DirectoryTree::HasManuallyCollapsedAncestor(const std::filesystem::path& path) const {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }

  const auto normalized_path = absolute_path.lexically_normal();
  for (auto current = normalized_path.parent_path();
       !current.empty() && current != root_ && current.native().rfind(root_.native(), 0) == 0;
       current = current.parent_path()) {
    if (manually_collapsed_paths_.contains(NormalizePathKey(current))) {
      return true;
    }
  }
  return false;
}

void DirectoryTree::ExpandSelection() {
  if (entries_.empty()) {
    return;
  }

  const auto& entry = entries_[selected_index_];
  if (!entry.is_directory) {
    return;
  }

  const auto key = NormalizePathKey(entry.path);
  if (expanded_paths_.insert(key).second) {
    manually_collapsed_paths_.erase(key);
    RebuildEntries(false);
    return;
  }

  if (selected_index_ + 1 < entries_.size()) {
    ++selected_index_;
  }
}

void DirectoryTree::CollapseSelection() {
  if (entries_.empty()) {
    return;
  }

  const auto& entry = entries_[selected_index_];
  if (entry.is_directory && entry.expanded && entry.path != root_) {
    manually_collapsed_paths_.insert(NormalizePathKey(entry.path));
    expanded_paths_.erase(NormalizePathKey(entry.path));
    RebuildEntries(false);
    return;
  }

  if (entry.depth == 0) {
    return;
  }

  for (std::size_t i = selected_index_; i > 0; --i) {
    const auto parent_index = i - 1;
    if (entries_[parent_index].depth == entry.depth - 1) {
      selected_index_ = parent_index;
      return;
    }
  }
}

void DirectoryTree::CollapseAll() {
  if (root_.empty()) {
    return;
  }

  const auto selected_path =
      entries_.empty() ? root_ : entries_[selected_index_].path.lexically_normal();
  manually_collapsed_paths_.clear();
  for (const auto& entry : entries_) {
    if (entry.is_directory && entry.path != root_) {
      manually_collapsed_paths_.insert(NormalizePathKey(entry.path));
    }
  }
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  RebuildEntries(false);

  std::filesystem::path visible_path = selected_path;
  while (!visible_path.empty()) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].path == visible_path) {
        selected_index_ = i;
        return;
      }
    }
    if (visible_path == root_) {
      break;
    }
    visible_path = visible_path.parent_path().lexically_normal();
  }

  selected_index_ = 0;
}

std::optional<std::filesystem::path> DirectoryTree::ActivateSelection() {
  if (entries_.empty()) {
    return std::nullopt;
  }

  const auto& entry = entries_[selected_index_];
  if (entry.is_directory) {
    const auto key = NormalizePathKey(entry.path);
    if (entry.expanded && entry.path != root_) {
      manually_collapsed_paths_.insert(key);
      expanded_paths_.erase(key);
    } else {
      manually_collapsed_paths_.erase(key);
      expanded_paths_.insert(key);
    }
    RebuildEntries(false);
    return std::nullopt;
  }

  return entry.path;
}

void DirectoryTree::RebuildEntries(bool refresh_git_statuses) {
  util::StartupTrace::Scope trace_scope("DirectoryTree::RebuildEntries");
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::RebuildEntries");
  ++entries_revision_;
  const auto selected_path =
      entries_.empty() ? std::filesystem::path{} : entries_[selected_index_].path;

  entries_.clear();
  if (root_.empty()) {
    git_statuses_.clear();
    selected_index_ = 0;
    return;
  }

  IgnoreMatcher matcher;
  matcher.SetRoot(root_);
  if (refresh_git_statuses) {
    git_statuses_ = CollectGitStatuses(root_);
  }
  entries_.push_back(TreeEntry{
      .path = root_,
      .label = DisplayName(root_),
      .depth = 0,
      .is_directory = true,
      .expanded = true,
      .ignored = false,
      .children_materialized = true,
      .git_status = GitFileStatus::Clean,
  });
  SymlinkLoopGuard loop_guard(root_);
  AppendDirectory(root_, 1, matcher, loop_guard);

  selected_index_ = 0;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == selected_path) {
      selected_index_ = i;
      break;
    }
  }
}

void DirectoryTree::AppendDirectory(const std::filesystem::path& directory,
                                    int depth,
                                    const IgnoreMatcher& matcher,
                                    SymlinkLoopGuard& loop_guard) {
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::AppendDirectory");
  if (depth > platform::kMaxTreeWalkDepth) {
    return;  // bound native-stack recursion on a pathologically deep tree
  }
  if (directory != root_ && !IsExpanded(directory)) {
    return;
  }

  std::error_code error;
  std::vector<SortableEntry> children;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto path = iterator->path();
    const std::string sort_key = path.filename().string();
    const auto relative = path.lexically_relative(root_);
    if (relative.empty()) {
      iterator.increment(error);
      continue;
    }

    std::error_code type_error;
    const bool is_directory = iterator->is_directory(type_error);
    if (type_error) {
      iterator.increment(error);
      continue;
    }
    if (is_directory && path.filename() == ".git") {
      iterator.increment(error);
      continue;
    }

    const bool ignored = matcher.Ignored(relative, is_directory);
    if (sort_key.starts_with('.') && !ignored) {
      iterator.increment(error);
      continue;
    }

    if (is_directory) {
      std::error_code link_error;
      const bool is_symlink = iterator->is_symlink(link_error);
      IgnoreMatcher child_matcher = matcher;
      child_matcher.LoadIgnoreFile(path / ".gitignore");
      children.push_back(SortableEntry{
          .path = path,
          .sort_key = sort_key,
          .is_directory = true,
          .is_symlink = is_symlink && !link_error,
          .ignored = ignored,
          .matcher = std::move(child_matcher),
      });
      iterator.increment(error);
      continue;
    }

    children.push_back(SortableEntry{
        .path = path,
        .sort_key = sort_key,
        .is_directory = false,
        .ignored = ignored,
        .matcher = IgnoreMatcher{},
    });
    iterator.increment(error);
  }

  std::sort(children.begin(), children.end(), [](const SortableEntry& lhs, const SortableEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
      return lhs.is_directory > rhs.is_directory;
    }
    return lhs.sort_key < rhs.sort_key;
  });

  for (const auto& child : children) {
    const bool expanded = child.is_directory && IsExpanded(child.path);
    entries_.push_back(TreeEntry{
        .path = child.path,
        .label = DisplayName(child.path),
        .depth = depth,
        .is_directory = child.is_directory,
        .expanded = expanded,
        .ignored = child.ignored,
        .children_materialized = expanded,
        .git_status = EntryGitStatus(child.path),
    });

    if (child.is_directory) {
      const SymlinkLoopGuard::Scope scope = loop_guard.TryEnter(child.path, child.is_symlink);
      if (scope.entered()) {
        AppendDirectory(child.path, depth + 1, child.matcher, loop_guard);
      }
    }
  }
}

namespace {

std::vector<std::string> RelativeKeysExcludingRoot(
    const std::unordered_set<std::string>& keys,
    const std::filesystem::path& root) {
  const std::string root_key = std::filesystem::absolute(root).lexically_normal().generic_string();
  std::vector<std::string> relative;
  relative.reserve(keys.size());
  for (const auto& key : keys) {
    if (key == root_key) {
      continue;
    }
    const auto rel = std::filesystem::path(key).lexically_relative(root);
    if (rel.empty() || rel == std::filesystem::path(".")) {
      continue;
    }
    relative.push_back(rel.generic_string());
  }
  // Deterministic order so persisted records are stable across saves.
  std::sort(relative.begin(), relative.end());
  return relative;
}

}  // namespace

std::vector<std::string> DirectoryTree::ExpandedRelativePaths() const {
  if (root_.empty()) {
    return {};
  }
  return RelativeKeysExcludingRoot(expanded_paths_, root_);
}

std::vector<std::string> DirectoryTree::ManuallyCollapsedRelativePaths() const {
  if (root_.empty()) {
    return {};
  }
  return RelativeKeysExcludingRoot(manually_collapsed_paths_, root_);
}

std::optional<std::filesystem::path> DirectoryTree::SelectedPath() const {
  if (entries_.empty() || selected_index_ >= entries_.size()) {
    return std::nullopt;
  }
  return entries_[selected_index_].path;
}

void DirectoryTree::RestoreExpansionState(const std::vector<std::string>& expanded_relative,
                                          const std::vector<std::string>& collapsed_relative) {
  if (root_.empty()) {
    return;
  }
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  for (const auto& relative : expanded_relative) {
    if (relative.empty()) {
      continue;
    }
    expanded_paths_.insert(NormalizePathKey(root_ / std::filesystem::path(relative)));
  }
  manually_collapsed_paths_.clear();
  for (const auto& relative : collapsed_relative) {
    if (relative.empty()) {
      continue;
    }
    manually_collapsed_paths_.insert(NormalizePathKey(root_ / std::filesystem::path(relative)));
  }
  RebuildEntries(false);
}

bool DirectoryTree::has_dirty_files() const {
  for (const auto& [_, status] : git_statuses_) {
    if (status != GitFileStatus::Clean) {
      return true;
    }
  }
  return false;
}

GitFileStatus DirectoryTree::EntryGitStatus(const std::filesystem::path& path) const {
  if (git_statuses_.empty()) {
    return GitFileStatus::Clean;
  }

  const std::filesystem::path relative = path.lexically_relative(root_);
  if (relative.empty() || relative == ".") {
    return GitFileStatus::Clean;
  }

  const auto it = git_statuses_.find(relative.lexically_normal().string());
  return it == git_statuses_.end() ? GitFileStatus::Clean : it->second;
}

bool DirectoryTree::IsExpanded(const std::filesystem::path& path) const {
  return expanded_paths_.contains(NormalizePathKey(path));
}

bool DirectoryTree::CanCollapseAll() const {
  return !root_.empty() && expanded_paths_.size() > 1;
}

std::string DirectoryTree::NormalizePathKey(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  std::string key =
      error ? path.lexically_normal().generic_string() : absolute.lexically_normal().generic_string();
#ifdef _WIN32
  key = util::ToLowerAscii(key);
#endif
  return key;
}

}  // namespace microide::project
