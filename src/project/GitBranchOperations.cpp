#include "project/GitBranchOperations.h"

#include <utility>

#include "project/GitRepository.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// A single user-facing sentence per outcome. The raw git output still reaches the
// output panel; this is the line the status surface shows.
std::string DefaultDetail(const GitOperationOutcome outcome, const std::string_view verb) {
  switch (outcome) {
    case GitOperationOutcome::Success:
      return {};
    case GitOperationOutcome::NothingToDo:
      return "Already up to date";
    case GitOperationOutcome::AuthFailed:
      return "Git credentials are required and could not be supplied";
    case GitOperationOutcome::NoUpstream:
      return "The current branch has no upstream branch";
    case GitOperationOutcome::NoRemote:
      return "No git remote is configured";
    case GitOperationOutcome::NonFastForward:
      return "Push rejected: the remote has commits you do not have";
    case GitOperationOutcome::DirtyWorktree:
      return "Local changes would be overwritten";
    case GitOperationOutcome::Conflict:
      return "Conflicts must be resolved first";
    case GitOperationOutcome::BadRef:
      return "No such branch or reference";
    case GitOperationOutcome::RepoLocked:
      return "Git repository is locked";
    case GitOperationOutcome::NetworkFailed:
      return "Could not reach the git remote";
    case GitOperationOutcome::TimedOut:
      return std::string("Git ").append(verb).append(" timed out and was aborted");
    case GitOperationOutcome::NotARepo:
      return "Not a git repository";
    case GitOperationOutcome::UnknownError:
    default:
      return std::string("Git ").append(verb).append(" failed");
  }
}

GitOperationReport MakeReport(const GitOperationOutcome outcome,
                              const std::string_view verb,
                              std::string output) {
  GitOperationReport report;
  report.outcome = outcome;
  report.output = std::move(output);
  util::TrimTrailingLineEndings(&report.output);
  report.detail = DefaultDetail(outcome, verb);
  // A generic failure is far more useful with git's own words than with ours.
  if (outcome == GitOperationOutcome::UnknownError && !report.output.empty()) {
    report.detail = report.output;
  }
  return report;
}

// Runs one git write command and folds the result into a report. Every operation in
// this file funnels through here so the timeout/classification contract cannot drift
// between them.
GitOperationReport RunOperation(const std::filesystem::path& repository_root,
                                std::vector<std::string> arguments,
                                const std::string_view verb) {
  if (repository_root.empty()) {
    return MakeReport(GitOperationOutcome::NotARepo, verb, {});
  }
  GitRepository repo(repository_root);
  if (!repo.IsValid()) {
    return MakeReport(GitOperationOutcome::NotARepo, verb, {});
  }
  // Network operations and checkout hooks can legitimately take a while, so use the
  // generous write cap. GIT_TERMINAL_PROMPT=0 (set in GitCommandUtil) means a
  // credential prompt fails immediately instead of consuming the whole budget.
  const GitRepository::CommandResult result =
      repo.Execute(arguments, false, internal::kGitWriteTimeoutMs);
  if (result.timed_out) {
    return MakeReport(GitOperationOutcome::TimedOut, verb, result.output);
  }
  return MakeReport(ClassifyGitOperationFailure(result.exit_code, result.output), verb,
                    result.output);
}

}  // namespace

bool GitOperationSucceeded(const GitOperationOutcome outcome) {
  return outcome == GitOperationOutcome::Success || outcome == GitOperationOutcome::NothingToDo;
}

GitOperationOutcome ClassifyGitOperationFailure(const int exit_code,
                                                const std::string_view output) {
  const std::string lowered = util::ToLowerAscii(std::string(output));

  if (exit_code == 0) {
    // git exits 0 for several no-op cases; report them as such so the UI does not
    // claim work was done. "up to date" covers both git's "Already up to date."
    // (pull/merge) and "Everything up-to-date" (push).
    if (Contains(lowered, "up to date") || Contains(lowered, "up-to-date")) {
      return GitOperationOutcome::NothingToDo;
    }
    return GitOperationOutcome::Success;
  }

  // Order matters: the more specific phrase wins. Auth is checked before network
  // because an auth failure over https also mentions the remote URL.
  if (Contains(lowered, "authentication failed") || Contains(lowered, "could not read username") ||
      Contains(lowered, "could not read password") || Contains(lowered, "permission denied") ||
      Contains(lowered, "terminal prompts disabled") ||
      Contains(lowered, "access denied") || Contains(lowered, "invalid credentials")) {
    return GitOperationOutcome::AuthFailed;
  }
  if (Contains(lowered, "no upstream branch") || Contains(lowered, "no upstream configured") ||
      Contains(lowered, "set-upstream") || Contains(lowered, "no tracking information")) {
    return GitOperationOutcome::NoUpstream;
  }
  if (Contains(lowered, "does not appear to be a git repository") ||
      Contains(lowered, "no such remote") || Contains(lowered, "no configured push destination") ||
      Contains(lowered, "'origin' does not appear")) {
    return GitOperationOutcome::NoRemote;
  }
  if (Contains(lowered, "non-fast-forward") || Contains(lowered, "fetch first") ||
      Contains(lowered, "updates were rejected")) {
    return GitOperationOutcome::NonFastForward;
  }
  if (Contains(lowered, "index.lock") || Contains(lowered, "cannot lock ref") ||
      Contains(lowered, "unable to create") ||
      Contains(lowered, "another git process seems to be running")) {
    return GitOperationOutcome::RepoLocked;
  }
  if (Contains(lowered, "conflict")) {
    return GitOperationOutcome::Conflict;
  }
  if (Contains(lowered, "would be overwritten") ||
      Contains(lowered, "your local changes to the following files") ||
      Contains(lowered, "cannot pull with rebase") ||
      Contains(lowered, "please commit your changes or stash them")) {
    return GitOperationOutcome::DirtyWorktree;
  }
  if (Contains(lowered, "already exists") || Contains(lowered, "did not match any file") ||
      Contains(lowered, "invalid reference") || Contains(lowered, "unknown revision") ||
      Contains(lowered, "is not a commit") || Contains(lowered, "pathspec")) {
    return GitOperationOutcome::BadRef;
  }
  if (Contains(lowered, "could not resolve host") || Contains(lowered, "connection timed out") ||
      Contains(lowered, "connection refused") || Contains(lowered, "network is unreachable") ||
      Contains(lowered, "unable to access")) {
    return GitOperationOutcome::NetworkFailed;
  }
  return GitOperationOutcome::UnknownError;
}

GitOperationReport SwitchGitBranch(const std::filesystem::path& repository_root,
                                   const std::string_view branch) {
  if (branch.empty()) {
    return MakeReport(GitOperationOutcome::BadRef, "switch", {});
  }
  // `--` terminates options so a branch named like a flag cannot inject one.
  return RunOperation(repository_root, {"switch", "--", std::string(branch)}, "switch");
}

GitOperationReport CreateGitBranch(const std::filesystem::path& repository_root,
                                   const std::string_view branch,
                                   const std::string_view start_point) {
  if (branch.empty()) {
    return MakeReport(GitOperationOutcome::BadRef, "branch create", {});
  }
  std::vector<std::string> arguments{"switch", "-c", std::string(branch)};
  if (!start_point.empty()) {
    arguments.emplace_back(start_point);
  }
  return RunOperation(repository_root, std::move(arguments), "branch create");
}

GitOperationReport RunGitRemoteOperation(const std::filesystem::path& repository_root,
                                         const GitRemoteOperationKind kind,
                                         const std::string_view branch,
                                         const bool set_upstream) {
  switch (kind) {
    case GitRemoteOperationKind::Fetch:
      // --prune keeps the remote-branch list from accumulating refs deleted upstream,
      // which is what makes the branch picker trustworthy after a fetch.
      return RunOperation(repository_root, {"fetch", "--prune"}, "fetch");
    case GitRemoteOperationKind::Pull:
      return RunOperation(repository_root, {"pull"}, "pull");
    case GitRemoteOperationKind::Push:
    default: {
      std::vector<std::string> arguments{"push"};
      if (set_upstream) {
        if (branch.empty()) {
          return MakeReport(GitOperationOutcome::NoUpstream, "push", {});
        }
        arguments.emplace_back("--set-upstream");
        arguments.emplace_back("origin");
        arguments.emplace_back(branch);
      }
      return RunOperation(repository_root, std::move(arguments), "push");
    }
  }
}

GitOperationReport StashGitChanges(const std::filesystem::path& repository_root,
                                   const std::string_view message,
                                   const bool include_untracked) {
  std::vector<std::string> arguments{"stash", "push"};
  if (include_untracked) {
    arguments.emplace_back("--include-untracked");
  }
  if (!message.empty()) {
    arguments.emplace_back("-m");
    arguments.emplace_back(message);
  }
  GitOperationReport report = RunOperation(repository_root, std::move(arguments), "stash");
  // `git stash push` with a clean tree exits 0 and says so; report it as a no-op
  // rather than letting the caller claim changes were stashed.
  if (report.outcome == GitOperationOutcome::Success &&
      util::ToLowerAscii(report.output).find("no local changes") != std::string::npos) {
    report.outcome = GitOperationOutcome::NothingToDo;
    report.detail = "No local changes to stash";
  }
  return report;
}

GitOperationReport PopGitStash(const std::filesystem::path& repository_root) {
  GitOperationReport report = RunOperation(repository_root, {"stash", "pop"}, "stash pop");
  if (!report.success() &&
      util::ToLowerAscii(report.output).find("no stash entries") != std::string::npos) {
    report.outcome = GitOperationOutcome::NothingToDo;
    report.detail = "No stash entries to pop";
  }
  return report;
}

}  // namespace microide::project
