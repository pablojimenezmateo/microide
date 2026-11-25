#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "project/PendingPaths.h"
#include "util/TransparentStringHash.h"

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

using GitTreeStatusMap = std::unordered_map<std::string, GitFileStatus>;
using SharedGitTreeStatusMap = std::shared_ptr<const GitTreeStatusMap>;

struct TreeEntry {
  std::filesystem::path path;
  std::string label;
  int depth = 0;
  bool is_directory = false;
  bool expanded = false;
  bool ignored = false;
  bool children_materialized = false;
  GitFileStatus git_status = GitFileStatus::Clean;
  bool pending = false;
};

class DirectoryTree {
 public:
  using PathKeySet =
      std::unordered_set<std::string, util::TransparentStringHash, std::equal_to<>>;

  bool SetRoot(const std::filesystem::path& root);
  void Refresh();
  void SetFollowOutOfRootSymlinks(bool follow) { follow_out_of_root_symlinks_ = follow; }
  void SetExcludeGlobs(std::vector<std::string> globs) { exclude_globs_ = std::move(globs); }
  [[nodiscard]] bool ApplyGitStatuses(SharedGitTreeStatusMap statuses);
  void SetPendingPaths(SharedPendingPaths pending);
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

  std::vector<std::string> ExpandedRelativePaths() const;
  std::vector<std::string> ManuallyCollapsedRelativePaths() const;
  std::optional<std::filesystem::path> SelectedPath() const;
  void RestoreExpansionState(const std::vector<std::string>& expanded_relative,
                             const std::vector<std::string>& collapsed_relative);

  std::uint64_t entries_revision() const { return entries_revision_; }

  bool has_dirty_files() const;

 private:
  void RebuildEntries();
  void AppendDirectory(const std::filesystem::path& directory,
                       int depth,
                       const std::shared_ptr<const IgnoreMatcher>& matcher,
                       SymlinkLoopGuard& loop_guard);
  struct SortableEntry;
  void AppendPendingChildren(const std::filesystem::path& directory,
                             const std::shared_ptr<const IgnoreMatcher>& matcher,
                             std::vector<SortableEntry>& children) const;
  GitFileStatus EntryGitStatus(const std::filesystem::path& path, std::string& scratch) const;
  bool IsExpanded(const std::filesystem::path& path) const;
  void PruneDeletedDirectoryKeys();
  void MaybePruneDeletedDirectoryKeys();
  static std::string NormalizePathKey(const std::filesystem::path& path);

  static bool ContainsPathKey(const PathKeySet& keys, const std::filesystem::path& path);

  std::filesystem::path root_;
  std::string root_generic_;
  bool follow_out_of_root_symlinks_ = false;
  std::vector<std::string> exclude_globs_;
  std::vector<TreeEntry> entries_;
  std::uint64_t entries_revision_ = 0;
  SharedGitTreeStatusMap git_statuses_;
  SharedPendingPaths pending_;
  PathKeySet expanded_paths_;
  PathKeySet manually_collapsed_paths_;
  std::size_t refreshes_since_key_prune_ = 0;
  std::size_t selected_index_ = 0;
};

}
