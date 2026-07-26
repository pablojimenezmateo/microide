#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

struct GitRepositoryPathIdentity {
  std::filesystem::path relative_path;
  std::string display_label;
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
  // Unmerged records carry a mode per stage (`u <XY> <sub> <m1> <m2> <m3> ...`).
  // True when OURS and THEIRS both exist with different modes — the executable-bit
  // conflict git otherwise reports indistinguishably from a content conflict.
  bool stage_modes_differ = false;
  // True when the worktree path for a CONFLICTED entry is a directory. That is
  // git's D/F (file-vs-directory) conflict: one side made the path a file, the
  // other a directory, and git leaves the directory in place while stashing the
  // file side under a suffixed name. Unlike `submodule` and `stage_modes_differ`
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

GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::filesystem::path relative_path);
GitRefreshErrorCategory ClassifyGitRefreshFailure(int exit_code, std::string_view stderr_text);
GitConflictKind ConflictKindFromUnmergedCodes(std::string_view xy);
GitFileStatus StatusFromPorcelainV2XY(std::string_view xy, bool conflicted);

}  // namespace microide::project
