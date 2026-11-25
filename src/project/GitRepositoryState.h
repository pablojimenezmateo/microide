#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/DirectoryTree.h"

namespace microide::project {

class GitMetadataSource;

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
  GitNotFound,
  UnknownError,
};

std::string DescribeSilentGitFailure(GitRefreshErrorCategory category, int exit_code);

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
  std::string relative_path;
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
  bool worktree_dirty = false;
  bool conflicted = false;
  bool submodule = false;
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
  SharedGitTreeStatusMap tree_git_statuses{};
  GitRepositoryRefreshError refresh_error{};
  GitOperationStateKind operation_state = GitOperationStateKind::None;
  std::string pending_merge_head;
  std::size_t worktree_count = 0;
  std::uint64_t generation = 0;
  std::uint64_t refreshed_at_ms = 0;
  bool stale = false;
  bool refreshing = false;
};

GitOperationStateKind DetectGitOperationState(const std::filesystem::path& repository_root,
                                              const GitMetadataSource& metadata);

std::size_t CountGitWorktrees(const std::filesystem::path& repository_root, const GitMetadataSource& metadata);

GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::string relative_path);

[[nodiscard]] inline std::string_view GenericPathView(const GitRepositoryPathIdentity& identity) {
  return identity.relative_path;
}

[[nodiscard]] inline std::string_view DisplayLabelView(const GitRepositoryPathIdentity& identity) {
  return identity.path_is_valid_utf8 ? std::string_view(identity.relative_path)
                                     : std::string_view(identity.escaped_label);
}
GitRefreshErrorCategory ClassifyGitRefreshFailure(int exit_code, std::string_view stderr_text);
GitConflictKind ConflictKindFromUnmergedCodes(std::string_view xy);
GitFileStatus StatusFromPorcelainV2XY(std::string_view xy, bool conflicted);

}
