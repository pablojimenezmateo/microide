#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::project {

enum class CommitPreCheckSeverity {
  Blocking,
  Warning,
};

enum class CommitPreCheckKind {
  EmptySubject,
  LongSubject,
  UnresolvedConflicts,
  ConflictMarkers,
  UnstagedLeftovers,
  BranchBehind,
  UntrackedFiles,
};

struct CommitPreCheck {
  CommitPreCheckKind kind = CommitPreCheckKind::EmptySubject;
  CommitPreCheckSeverity severity = CommitPreCheckSeverity::Blocking;
  std::string id;
  std::string message;
};

struct CommitStagedFileSummary {
  std::filesystem::path relative_path;
  int added_lines = 0;
  int deleted_lines = 0;
};

struct CommitStagedSummary {
  std::size_t file_count = 0;
  int added_lines = 0;
  int deleted_lines = 0;
  std::vector<CommitStagedFileSummary> files;
};

enum class CommitOperationKind {
  Create,
  Amend,
  NoVerify,
};

enum class CommitOperationResultCategory {
  Success,
  Cancelled,
  HookFailed,
  DirtyWorktree,
  Conflict,
  AuthFailed,
  RepoLocked,
  TimedOut,
  RefreshFailedAfterSuccess,
  UnknownError,
};

struct CommitOperationResult {
  CommitOperationResultCategory category = CommitOperationResultCategory::UnknownError;
  std::string detail;
  std::string hook_output;
  bool refresh_failed_after_success = false;
};

struct CommitDraftContext {
  std::string head_oid;
  std::string branch_name;
};

constexpr std::size_t kCommitSubjectMaxLength = 72;

}  // namespace microide::project
