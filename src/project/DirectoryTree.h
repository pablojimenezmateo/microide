#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// Per-path (and per-ancestor-directory) badge status for the file tree, keyed by
// repository-relative generic ('/'-separated) text.
using GitTreeStatusMap = std::unordered_map<std::string, GitFileStatus>;
// …and the shared, immutable form the git refresh pipeline passes around.
//
// The map's whole life is: built once by the porcelain parser, read into the
// sidebar refresh snapshot, adopted here. Nothing mutates it after the parse, and
// nothing but the snapshot ever read it off the published repository state — yet
// building the snapshot deep-copied it, a node and a key string per changed path
// (2,018 allocations on a 1,000-file refresh) for a map that was about to be moved
// on anyway. A null pointer means "no statuses".
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
};

class DirectoryTree {
 public:
  // Expansion-state keys are normalized absolute path text. `NormalizePathKey`
  // materializes that text through `absolute()` (a getcwd syscall) plus
  // `lexically_normal()` (~12 allocations) plus `generic_string()` — and the
  // tree rebuild probes it once per candidate entry, on paths it built itself by
  // appending a filename to its own absolute, normal root. For those the key IS
  // the path's own spelling, so `ContainsPathKey` probes the set through a view
  // into it (TD-2026-08-10-174), falling back to the authoritative form for
  // anything unusually spelled.
  using PathKeySet =
      std::unordered_set<std::string, util::TransparentStringHash, std::equal_to<>>;

  bool SetRoot(const std::filesystem::path& root);
  void Refresh();
  // Mirrors the `project.follow_out_of_root_symlinks` user setting; consulted
  // when the tree is rebuilt. Default false keeps the out-of-root containment
  // guard active.
  void SetFollowOutOfRootSymlinks(bool follow) { follow_out_of_root_symlinks_ = follow; }
  // User/project-configured ignore globs folded into the tree's matcher alongside
  // the built-in defaults; consulted on the next rebuild. Matched entries render
  // grayed (ignored), not hidden.
  void SetExcludeGlobs(std::vector<std::string> globs) { exclude_globs_ = std::move(globs); }
  void ApplyGitStatuses(SharedGitTreeStatusMap statuses);
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
                       const std::shared_ptr<const IgnoreMatcher>& matcher,
                       SymlinkLoopGuard& loop_guard);
  // Resolve a tree entry's git badge. `scratch` is a caller-owned buffer that the
  // key is built into: this runs once per entry on every git refresh, so a fresh
  // string (let alone the lexically_relative + lexically_normal + generic_string
  // chain it replaced) per entry is the whole cost of the sweep.
  GitFileStatus EntryGitStatus(const std::filesystem::path& path, std::string& scratch) const;
  bool IsExpanded(const std::filesystem::path& path) const;
  // Drop expanded/collapsed keys whose directory no longer exists on disk, so the
  // sets do not grow unbounded across a session and a deleted-then-recreated dir
  // renders collapsed (like VSCode) rather than pre-expanded. Called from the
  // fs-resync entry point (Refresh) only.
  void PruneDeletedDirectoryKeys();
  // Refresh runs on the UI thread and can fire on many events (save, git refresh,
  // external change). The full PruneDeletedDirectoryKeys sweep stats every remembered
  // expand/collapse key — O(session history), not O(visible/changed rows) — so after a
  // large session restore or lots of manual expansion it dominates refresh cost. Stale
  // keys are harmless to rendering (a deleted dir is simply never enumerated), so amortize:
  // sweep every refresh only while the set is small, and otherwise at a bounded interval
  // (TD-2026-07-17A-088).
  void MaybePruneDeletedDirectoryKeys();
  static std::string NormalizePathKey(const std::filesystem::path& path);

  static bool ContainsPathKey(const PathKeySet& keys, const std::filesystem::path& path);

  std::filesystem::path root_;
  // root_ in generic ('/'-separated) form, cached because every git-badge lookup
  // strips it as a prefix. Kept in lockstep with root_ (SetRoot is its only writer).
  std::string root_generic_;
  bool follow_out_of_root_symlinks_ = false;
  std::vector<std::string> exclude_globs_;
  std::vector<TreeEntry> entries_;
  std::uint64_t entries_revision_ = 0;
  SharedGitTreeStatusMap git_statuses_;
  PathKeySet expanded_paths_;
  PathKeySet manually_collapsed_paths_;
  // Refreshes elapsed since the last full stale-key sweep (see MaybePruneDeletedDirectoryKeys).
  std::size_t refreshes_since_key_prune_ = 0;
  std::size_t selected_index_ = 0;
};

}  // namespace microide::project
