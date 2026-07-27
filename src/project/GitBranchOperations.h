#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

// Why a git write operation ended the way it did. Kept coarse and closed so the
// UI can pick a specific, actionable message instead of dumping raw git output —
// the same contract ClassifyCommitFailure established for commits.
enum class GitOperationOutcome {
  Success,
  // The command succeeded but changed nothing ("Already up to date", "No stash
  // entries", "Everything up-to-date"). Distinct from Success so the UI can say so
  // rather than implying work happened.
  NothingToDo,
  // Credentials were required and could not be supplied non-interactively.
  AuthFailed,
  // Push/pull with no configured upstream for the current branch.
  NoUpstream,
  // No remote is configured at all.
  NoRemote,
  // Push rejected because the remote has commits we do not have.
  NonFastForward,
  // Local modifications would be overwritten (switch/pull/stash-pop).
  DirtyWorktree,
  // Merge or stash-pop produced conflicts.
  Conflict,
  // The named branch/ref does not exist, or already exists on create.
  BadRef,
  // Another git process holds the index/ref lock.
  RepoLocked,
  // Network unreachable / host lookup failure.
  NetworkFailed,
  TimedOut,
  NotARepo,
  UnknownError,
};

bool GitOperationSucceeded(GitOperationOutcome outcome);

// Result of one git write operation. `detail` is a single user-facing sentence;
// `output` is the raw captured git text for the output panel (may be empty).
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

// Pure classifier over a git invocation's exit code and captured output. Static and
// side-effect free so every failure mapping is unit-testable without spawning git.
GitOperationOutcome ClassifyGitOperationFailure(int exit_code, std::string_view output);

// Branch enumeration lives in project::CollectGitBranches (GitCompareService.h) —
// one date-sorted, capped, HEAD-aware list shared with the compare pickers rather
// than a second for-each-ref parser here.

// `git switch <branch>`. Refuses to run with a dirty index/worktree conflict rather
// than clobbering local work — git's own guard, surfaced as DirtyWorktree.
GitOperationReport SwitchGitBranch(const std::filesystem::path& repository_root,
                                   std::string_view branch);

// `git switch -c <branch> [start_point]`. An empty `start_point` branches from HEAD.
GitOperationReport CreateGitBranch(const std::filesystem::path& repository_root,
                                   std::string_view branch,
                                   std::string_view start_point = {});

// Fetch/pull/push against the default remote. Push publishes an unpublished branch
// with `--set-upstream` when `set_upstream` is true, which is what "Publish Branch"
// means in a GUI client; otherwise a missing upstream reports NoUpstream so the
// caller can offer to publish.
GitOperationReport RunGitRemoteOperation(const std::filesystem::path& repository_root,
                                         GitRemoteOperationKind kind,
                                         std::string_view branch = {},
                                         bool set_upstream = false);

// `git stash push` (optionally including untracked files) and `git stash pop`.
GitOperationReport StashGitChanges(const std::filesystem::path& repository_root,
                                   std::string_view message,
                                   bool include_untracked);
GitOperationReport PopGitStash(const std::filesystem::path& repository_root);

}  // namespace microide::project
