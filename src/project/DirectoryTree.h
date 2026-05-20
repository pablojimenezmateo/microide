#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace microide::project {

class IgnoreMatcher;

enum class GitFileStatus {
  Clean,
  Modified,
  Added,
  Deleted,
  Untracked,
  Conflicted,
};

struct TreeEntry {
  std::filesystem::path path;
  std::string label;
  int depth = 0;
  bool is_directory = false;
  bool expanded = false;
  bool ignored = false;
  bool children_materialized = false;
  GitFileStatus git_status = GitFileStatus::Clean;
};

class DirectoryTree {
 public:
  bool SetRoot(const std::filesystem::path& root);
  void Refresh();
  void RefreshGitStatuses();
  void ApplyGitStatuses(std::unordered_map<std::string, GitFileStatus> statuses);
  void MoveSelection(int delta);
  void SetSelectedIndex(std::size_t index);
  bool SelectPath(const std::filesystem::path& path);
  bool SelectPathIfVisible(const std::filesystem::path& path);
  bool HasManuallyCollapsedAncestor(const std::filesystem::path& path) const;
  void ExpandSelection();
  void CollapseSelection();
  void CollapseAll();
  std::optional<std::filesystem::path> ActivateSelection();
  bool CanCollapseAll() const;

  const std::filesystem::path& root() const { return root_; }
  const std::vector<TreeEntry>& entries() const { return entries_; }
  std::size_t selected_index() const { return selected_index_; }

  // Whether any file in the repo (not just the currently visible tree rows)
  // is non-Clean per the last RefreshGitStatuses/ApplyGitStatuses update.
  // Iterating entries() alone misses dirty files inside collapsed folders, so
  // callers that want a repo-wide signal should consult this.
  bool has_dirty_files() const;

 private:
  void RebuildEntries(bool refresh_git_statuses);
  void AppendDirectory(const std::filesystem::path& directory,
                       int depth,
                       const IgnoreMatcher& matcher);
  GitFileStatus EntryGitStatus(const std::filesystem::path& path) const;
  bool IsExpanded(const std::filesystem::path& path) const;
  static std::string NormalizePathKey(const std::filesystem::path& path);

  std::filesystem::path root_;
  std::vector<TreeEntry> entries_;
  std::unordered_map<std::string, GitFileStatus> git_statuses_;
  std::unordered_set<std::string> expanded_paths_;
  std::unordered_set<std::string> manually_collapsed_paths_;
  std::size_t selected_index_ = 0;
};

}  // namespace microide::project
