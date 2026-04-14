#include "project/DirectoryTree.h"

#include <algorithm>
#include <system_error>

#include "project/GitStatusService.h"
#include "project/IgnoreMatcher.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

struct SortableEntry {
  std::filesystem::path path;
  std::string sort_key;
  bool is_directory = false;
  IgnoreMatcher matcher;
};

std::string DisplayName(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  if (!name.empty()) {
    return name;
  }
  return path.string();
}

bool IsHidden(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return !name.empty() && name[0] == '.';
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
  RebuildEntries(true);
  return true;
}

void DirectoryTree::Refresh() {
  if (root_.empty()) {
    return;
  }
  RebuildEntries(true);
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

  const auto relative = std::filesystem::relative(normalized_path, root_, error);
  if (error || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return false;
  }

  for (auto current = normalized_path.parent_path();
       !current.empty() && current != root_ && current.native().rfind(root_.native(), 0) == 0;
       current = current.parent_path()) {
    expanded_paths_.insert(NormalizePathKey(current));
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
      expanded_paths_.erase(key);
    } else {
      expanded_paths_.insert(key);
    }
    RebuildEntries(false);
    return std::nullopt;
  }

  return entry.path;
}

void DirectoryTree::RebuildEntries(bool refresh_git_statuses) {
  util::StartupTrace::Scope trace_scope("DirectoryTree::RebuildEntries");
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
      .git_status = GitFileStatus::Clean,
  });
  AppendDirectory(root_, 1, matcher);

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
                                    const IgnoreMatcher& matcher) {
  if (!IsExpanded(directory)) {
    return;
  }

  std::error_code error;
  std::vector<SortableEntry> children;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      break;
    }

    const auto path = entry.path();
    const std::string sort_key = path.filename().string();
    std::error_code relative_error;
    const auto relative = std::filesystem::relative(path, root_, relative_error);
    if (relative_error || relative.empty()) {
      continue;
    }

    const bool is_directory = entry.is_directory();
    if (is_directory && path.filename() == ".git") {
      continue;
    }

    if (IsHidden(path)) {
      continue;
    }

    if (is_directory) {
      IgnoreMatcher child_matcher = matcher;
      child_matcher.LoadIgnoreFile(path / ".gitignore");
      if (child_matcher.Ignored(relative, true)) {
        continue;
      }
      children.push_back(SortableEntry{
          .path = path,
          .sort_key = sort_key,
          .is_directory = true,
          .matcher = std::move(child_matcher),
      });
      continue;
    }

    if (matcher.Ignored(relative, false)) {
      continue;
    }

    children.push_back(SortableEntry{
        .path = path,
        .sort_key = sort_key,
        .is_directory = false,
        .matcher = IgnoreMatcher{},
    });
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
        .git_status = EntryGitStatus(child.path),
    });

    if (child.is_directory) {
      AppendDirectory(child.path, depth + 1, child.matcher);
    }
  }
}

GitFileStatus DirectoryTree::EntryGitStatus(const std::filesystem::path& path) const {
  if (git_statuses_.empty()) {
    return GitFileStatus::Clean;
  }

  std::error_code error;
  std::filesystem::path relative = std::filesystem::relative(path, root_, error);
  if (error || relative.empty() || relative == ".") {
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
  return error ? path.lexically_normal().string() : absolute.lexically_normal().string();
}

}  // namespace microide::project
