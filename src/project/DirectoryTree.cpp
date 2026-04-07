#include "project/DirectoryTree.h"

#include <algorithm>
#include <system_error>

#include "project/GitStatusService.h"
#include "project/IgnoreMatcher.h"

namespace microide::project {

namespace {

struct SortableEntry {
  std::filesystem::path path;
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
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }

  root_ = absolute_root;
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  RebuildEntries();
  return true;
}

void DirectoryTree::Refresh() {
  if (root_.empty()) {
    return;
  }
  RebuildEntries();
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

  RebuildEntries();
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
    RebuildEntries();
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
    RebuildEntries();
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
    RebuildEntries();
    return std::nullopt;
  }

  return entry.path;
}

void DirectoryTree::RebuildEntries() {
  const auto selected_path =
      entries_.empty() ? std::filesystem::path{} : entries_[selected_index_].path;

  entries_.clear();
  git_statuses_.clear();
  if (root_.empty()) {
    selected_index_ = 0;
    return;
  }

  IgnoreMatcher matcher;
  matcher.SetRoot(root_);
  git_statuses_ = CollectGitStatuses(root_);
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
    std::error_code relative_error;
    const auto relative = std::filesystem::relative(path, root_, relative_error);
    if (relative_error || relative.empty()) {
      continue;
    }

    const bool is_directory = entry.is_directory();
    if (is_directory && path.filename() == ".git") {
      continue;
    }

    IgnoreMatcher child_matcher = matcher;
    if (is_directory) {
      child_matcher.LoadIgnoreFile(path / ".gitignore");
    }

    if (child_matcher.Ignored(relative, is_directory) || IsHidden(path)) {
      continue;
    }

    children.push_back(SortableEntry{
        .path = path,
        .is_directory = is_directory,
        .matcher = std::move(child_matcher),
    });
  }

  std::sort(children.begin(), children.end(), [](const SortableEntry& lhs, const SortableEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
      return lhs.is_directory > rhs.is_directory;
    }
    return lhs.path.filename().string() < rhs.path.filename().string();
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

std::string DirectoryTree::NormalizePathKey(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  return error ? path.lexically_normal().string() : absolute.lexically_normal().string();
}

}  // namespace microide::project
