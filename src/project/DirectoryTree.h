#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace microide::project {

class IgnoreMatcher;
class SymlinkLoopGuard;

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
  // Mirrors the `project.follow_out_of_root_symlinks` user setting; consulted
  // when the tree is rebuilt. Default false keeps the out-of-root containment
  // guard active.
  void SetFollowOutOfRootSymlinks(bool follow) { follow_out_of_root_symlinks_ = follow; }
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

  // Session-persistence accessors. Expansion/collapse state is reported as paths
  // relative to root() (portable if the project directory moves); the root itself
  // is omitted. RestoreExpansionState repopulates the in-memory sets and rebuilds
  // the visible rows. Call it after SetRoot() and before the first Refresh().
  std::vector<std::string> ExpandedRelativePaths() const;
  std::vector<std::string> ManuallyCollapsedRelativePaths() const;
  std::optional<std::filesystem::path> SelectedPath() const;
  void RestoreExpansionState(const std::vector<std::string>& expanded_relative,
                             const std::vector<std::string>& collapsed_relative);

  // Monotonic counter bumped whenever entries_ is rebuilt (set-root, refresh,
  // expand/collapse, activate). Lets render-side caches keyed on the entries
  // vector invalidate without diffing it. Git-status updates do not bump this:
  // they recolour labels but never change icon resolution.
  std::uint64_t entries_revision() const { return entries_revision_; }

  // Whether any file in the repo (not just the currently visible tree rows)
  // is non-Clean per the last RefreshGitStatuses/ApplyGitStatuses update.
  // Iterating entries() alone misses dirty files inside collapsed folders, so
  // callers that want a repo-wide signal should consult this.
  bool has_dirty_files() const;

 private:
  void RebuildEntries(bool refresh_git_statuses);
  void AppendDirectory(const std::filesystem::path& directory,
                       int depth,
                       const IgnoreMatcher& matcher,
                       SymlinkLoopGuard& loop_guard);
  GitFileStatus EntryGitStatus(const std::filesystem::path& path) const;
  bool IsExpanded(const std::filesystem::path& path) const;
  static std::string NormalizePathKey(const std::filesystem::path& path);

  std::filesystem::path root_;
  bool follow_out_of_root_symlinks_ = false;
  std::vector<TreeEntry> entries_;
  std::uint64_t entries_revision_ = 0;
  std::unordered_map<std::string, GitFileStatus> git_statuses_;
  std::unordered_set<std::string> expanded_paths_;
  std::unordered_set<std::string> manually_collapsed_paths_;
  std::size_t selected_index_ = 0;
};

}  // namespace microide::project
