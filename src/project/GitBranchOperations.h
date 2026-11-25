#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"

namespace microide::project {

enum class GitOperationOutcome {
  Success,
  NothingToDo,
  AuthFailed,
  NoUpstream,
  NoRemote,
  NonFastForward,
  DirtyWorktree,
  Conflict,
  BadRef,
  RepoLocked,
  NetworkFailed,
  TimedOut,
  NotARepo,
  GitNotFound,
  UnknownError,
};

bool GitOperationSucceeded(GitOperationOutcome outcome);

struct GitOperationReport {
  GitOperationOutcome outcome = GitOperationOutcome::UnknownError;
  std::string detail;
  std::string output;

  bool success() const { return GitOperationSucceeded(outcome); }
};

enum class GitRemoteOperationKind {
  Fetch,
  Pull,
  Push,
};

GitOperationOutcome ClassifyGitOperationFailure(int exit_code, std::string_view output);

GitOperationReport SwitchGitBranch(const std::filesystem::path& repository_root,
                                   const platform::ProcessLauncher& launcher,
                                   std::string_view branch);

GitOperationReport CreateGitBranch(const std::filesystem::path& repository_root,
                                   const platform::ProcessLauncher& launcher,
                                   std::string_view branch,
                                   std::string_view start_point = {});

GitOperationReport RunGitRemoteOperation(const std::filesystem::path& repository_root,
                                         const platform::ProcessLauncher& launcher,
                                         GitRemoteOperationKind kind,
                                         std::string_view branch = {},
                                         bool set_upstream = false);

GitOperationReport StashGitChanges(const std::filesystem::path& repository_root,
                                   const platform::ProcessLauncher& launcher,
                                   std::string_view message,
                                   bool include_untracked);
GitOperationReport PopGitStash(const std::filesystem::path& repository_root,
                               const platform::ProcessLauncher& launcher);

enum class GitConflictSide { Current, Incoming };

GitOperationReport ResolveGitConflictWithSide(const std::filesystem::path& repository_root,
                                              const platform::ProcessLauncher& launcher,
                                              std::string_view relative_path,
                                              GitConflictSide side);

}
