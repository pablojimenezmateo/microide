#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/DirectoryTree.h"

namespace microide::project {

enum class GitRepositoryEntryKind {
  Ordinary,
  Renamed,
  Unmerged,
  Untracked,
  Ignored,
};

enum class GitConflictKind {
  None,
  BothModified,
  BothAdded,
  BothDeleted,
  DeletedByUs,
  DeletedByThem,
  AddedByUs,
  AddedByThem,
  Unknown,
};

enum class GitRefreshErrorCategory {
  None,
  NotARepo,
  RepoLocked,
  Cancelled,
  AuthFailed,
  SubmoduleError,
  UnknownError,
};

enum class GitOperationStateKind {
  None,
  Merge,
  Rebase,
  CherryPick,
  Revert,
  Bisect,
};

enum class GitHeadKind {
  Normal,
  Detached,
  Unborn,
};

enum class GitOperationResultCategory {
  Success,
  Cancelled,
  StaleGeneration,
  RepoLocked,
  UnknownError,
};

// A changed file's identity as the '/'-separated generic TEXT git reported, not as
// a `std::filesystem::path`. libstdc++'s `path` is `_M_pathname` plus `_M_cmpts` (a
// component list the constructor builds eagerly), so holding one here cost two-plus
// allocations per changed file before anything downstream asked for a path — and
// nothing downstream does: the tree-status map, the sidebar tree, the grouping keys
// and the row labels all index by this text, and only "open this file" wants a real
// path, which it builds at the point of use (TD-2026-08-11-183).
struct GitRepositoryPathIdentity {
  // Normalized generic text, relative to the repository root.
  std::string relative_path;
  // Hex escape of a path git reported with non-UTF-8 bytes. Empty when the path is
  // valid UTF-8 — in that case the display label IS `relative_path`, and storing a
  // second copy of it was a per-entry allocation the refresh path did not need.
  std::string escaped_label;
  bool path_is_valid_utf8 = true;
};

struct GitRepositoryBranchInfo {
  GitHeadKind head_kind = GitHeadKind::Normal;
  std::string head_oid;
  std::string branch_name;
  std::string upstream;
  int ahead = 0;
  int behind = 0;
};

struct GitRepositoryEntry {
  GitRepositoryEntryKind kind = GitRepositoryEntryKind::Ordinary;
  GitFileStatus status = GitFileStatus::Clean;
  GitConflictKind conflict_kind = GitConflictKind::None;
  GitRepositoryPathIdentity path;
  std::optional<GitRepositoryPathIdentity> old_path = std::nullopt;
  bool staged = false;
  // True when the worktree copy has changes beyond what is staged (porcelain v2 `Y`
  // status bit is not `.`). A single record can be both `staged` and `worktree_dirty`
  // (e.g. `1 MM` — staged edit plus further unstaged edits), which is exactly the
  // partial-stage case the commit surface warns about.
  bool worktree_dirty = false;
  bool conflicted = false;
  // Porcelain v2 `<sub>` field: "N..." for an ordinary path, "S<c><m><u>" for a
  // gitlink. A conflicted submodule is not a text merge — there is nothing to
  // three-way — so the merge surface must say so rather than offer hunks.
  bool submodule = false;
  // True when the worktree path for a CONFLICTED entry is a directory. That is
  // git's D/F (file-vs-directory) conflict: one side made the path a file, the
  // other a directory, and git leaves the directory in place while stashing the
  // file side under a suffixed name. Unlike `submodule`
  // this is NOT on the porcelain wire — porcelain v2 reports the path with a
  // missing stage and says nothing about the directory — so it is filled in by a
  // filesystem probe on the background refresh, never in the parser.
  bool path_is_directory = false;
};

struct GitRepositoryRefreshError {
  GitRefreshErrorCategory category = GitRefreshErrorCategory::None;
  std::string detail;
};

struct GitRepositoryState {
  std::filesystem::path repository_root;
  bool repo_available = false;
  GitRepositoryBranchInfo branch{};
  std::vector<GitRepositoryEntry> entries{};
  std::unordered_map<std::string, GitFileStatus> tree_git_statuses{};
  GitRepositoryRefreshError refresh_error{};
  GitOperationStateKind operation_state = GitOperationStateKind::None;
  std::uint64_t generation = 0;
  std::uint64_t refreshed_at_ms = 0;
  bool stale = false;
  bool refreshing = false;
};

// Which multi-step git operation (if any) is mid-flight, derived from the marker
// files git itself writes into the git directory (MERGE_HEAD, rebase-merge/,
// rebase-apply/, CHERRY_PICK_HEAD, REVERT_HEAD, BISECT_LOG). Pure filesystem
// probes — no subprocess — so the refresh path can fill this in without a second
// `git` invocation. `GitRepositoryState::operation_state` was declared but never
// written by anything, which left the merge resolver's rebase/cherry-pick
// "upstream" label unreachable.
GitOperationStateKind DetectGitOperationState(const std::filesystem::path& repository_root);

// `relative_path` is the generic text git already emits, so the common case is one
// string move and a UTF-8 scan; only an unnormalized spelling pays a path round-trip.
GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::string relative_path);

// The identity's normalized generic-form ('/'-separated) path — the key form the
// tree-status map and DirectoryTree index by. It is the stored text itself.
[[nodiscard]] inline std::string_view GenericPathView(const GitRepositoryPathIdentity& identity) {
  return identity.relative_path;
}

// What to SHOW for this path: the generic text, unless git reported bytes that are
// not valid UTF-8, in which case the escaped form stands in for it.
[[nodiscard]] inline std::string_view DisplayLabelView(const GitRepositoryPathIdentity& identity) {
  return identity.path_is_valid_utf8 ? std::string_view(identity.relative_path)
                                     : std::string_view(identity.escaped_label);
}
GitRefreshErrorCategory ClassifyGitRefreshFailure(int exit_code, std::string_view stderr_text);
GitConflictKind ConflictKindFromUnmergedCodes(std::string_view xy);
GitFileStatus StatusFromPorcelainV2XY(std::string_view xy, bool conflicted);

}  // namespace microide::project
