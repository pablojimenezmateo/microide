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

GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::filesystem::path relative_path);
GitRefreshErrorCategory ClassifyGitRefreshFailure(int exit_code, std::string_view stderr_text);
GitConflictKind ConflictKindFromUnmergedCodes(std::string_view xy);
GitFileStatus StatusFromPorcelainV2XY(std::string_view xy, bool conflicted);

}  // namespace microide::project
