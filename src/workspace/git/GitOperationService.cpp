#include "workspace/git/GitOperationService.h"

#include <utility>

namespace microide::workspace {

namespace {

constexpr std::string_view kOutputChannelId = "git.operation";
constexpr std::string_view kOutputChannelLabel = "Git";

// An outcome the user should be nudged about vs. one that is simply informational.
// NoUpstream is a Warning rather than an Error: it is the expected state of a
// freshly created branch and the fix is one action away (publish).
NotificationService::Tone ToneFor(const project::GitOperationOutcome outcome) {
  switch (outcome) {
    case project::GitOperationOutcome::Success:
    case project::GitOperationOutcome::NothingToDo:
      return NotificationService::Tone::Info;
    case project::GitOperationOutcome::NoUpstream:
    case project::GitOperationOutcome::NoRemote:
    case project::GitOperationOutcome::NonFastForward:
    case project::GitOperationOutcome::DirtyWorktree:
      return NotificationService::Tone::Warning;
    default:
      return NotificationService::Tone::Error;
  }
}

// True when the operation may have moved HEAD, the index, or the working tree, so
// the git sidebar and any clean open buffer must be re-read. Fetch only writes
// remote-tracking refs, but the sidebar's ahead/behind counts are derived from
// them, so it refreshes too — it just does not need a buffer reload.
bool TouchesWorkingTree(const project::GitOperationOutcome outcome) {
  return outcome == project::GitOperationOutcome::Success ||
         outcome == project::GitOperationOutcome::Conflict;
}

}  // namespace

GitOperationService::GitOperationService(project::ProjectBackgroundExecutor& background_executor)
    : background_executor_(background_executor) {}

void GitOperationService::SetCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void GitOperationService::SetCompletionWakeEvent(const std::uint32_t event_type) {
  completion_mailbox_.SetWakeEventType(event_type);
}

void GitOperationService::DrainCompletions() { completion_mailbox_.Drain(); }

bool GitOperationService::Dispatch(const std::filesystem::path& repository_root,
                                   std::string label,
                                   std::string success_message,
                                   std::function<project::GitOperationReport()> work) {
  if (repository_root.empty()) {
    return false;
  }
  // Reject rather than queue: two concurrent git writes would race the index lock,
  // and the user could not tell which one produced the result they see.
  bool expected = false;
  if (!in_flight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }

  status_message_ = std::move(label);
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    generation = ++operation_generation_;
  }
  if (callbacks_.request_redraw != nullptr) {
    callbacks_.request_redraw();
  }

  background_executor_.Post([this, generation, work = std::move(work),
                             success_message = std::move(success_message)]() mutable {
    // Worker thread: run git and produce a value. Every field the main thread will
    // read is carried in the report; nothing here touches service state.
    project::GitOperationReport report = work();
    completion_mailbox_.Post([this, generation, success_message = std::move(success_message),
                              report = std::move(report)]() mutable {
      Publish(generation, std::move(success_message), std::move(report));
    });
  });
  return true;
}

void GitOperationService::Publish(const std::uint64_t generation,
                                  std::string success_message,
                                  project::GitOperationReport report) {
  {
    std::lock_guard lock(mutex_);
    if (generation != operation_generation_) {
      return;  // Superseded (project switched / service reset); drop the stale result.
    }
  }
  status_message_.clear();
  in_flight_.store(false, std::memory_order_release);

  if (!report.output.empty() && callbacks_.append_output != nullptr) {
    callbacks_.append_output(kOutputChannelId, kOutputChannelLabel, report.output);
    // Only surface the panel when something went wrong: a successful fetch/push
    // prints progress chatter the user did not ask to look at.
    if (!report.success() && callbacks_.show_output_panel != nullptr) {
      callbacks_.show_output_panel(kOutputChannelId);
    }
  }

  if (callbacks_.notify != nullptr) {
    // `detail` is empty exactly on a clean success, where the caller's specific
    // "Switched to <branch>" reads far better than a generic acknowledgement.
    std::string message = report.detail.empty() ? std::move(success_message)
                                                : std::move(report.detail);
    if (message.empty()) {
      message = "Git operation finished";
    }
    callbacks_.notify(ToneFor(report.outcome), std::move(message));
  }

  if (report.success() || TouchesWorkingTree(report.outcome)) {
    if (callbacks_.request_git_refresh != nullptr) {
      callbacks_.request_git_refresh();
    }
    if (callbacks_.reload_open_buffers != nullptr) {
      callbacks_.reload_open_buffers();
    }
  }
  if (callbacks_.request_redraw != nullptr) {
    callbacks_.request_redraw();
  }
}

bool GitOperationService::SwitchBranch(const std::filesystem::path& repository_root,
                                       std::string branch) {
  if (branch.empty()) {
    return false;
  }
  std::string success = "Switched to " + branch;
  return Dispatch(repository_root, "Switching branch…", std::move(success),
                  [repository_root, branch = std::move(branch)]() {
                    return project::SwitchGitBranch(repository_root, branch);
                  });
}

bool GitOperationService::CreateBranch(const std::filesystem::path& repository_root,
                                       std::string branch,
                                       std::string start_point) {
  if (branch.empty()) {
    return false;
  }
  std::string success = "Created and switched to " + branch;
  return Dispatch(repository_root, "Creating branch…", std::move(success),
                  [repository_root, branch = std::move(branch),
                   start_point = std::move(start_point)]() {
                    return project::CreateGitBranch(repository_root, branch, start_point);
                  });
}

bool GitOperationService::Fetch(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Fetching…", "Fetched from remote", [repository_root]() {
    return project::RunGitRemoteOperation(repository_root,
                                          project::GitRemoteOperationKind::Fetch);
  });
}

bool GitOperationService::Pull(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Pulling…", "Pulled from remote", [repository_root]() {
    return project::RunGitRemoteOperation(repository_root, project::GitRemoteOperationKind::Pull);
  });
}

bool GitOperationService::Push(const std::filesystem::path& repository_root,
                               std::string branch,
                               const bool set_upstream) {
  if (set_upstream && branch.empty()) {
    return false;
  }
  const std::string success = set_upstream ? "Published " + branch : "Pushed to remote";
  return Dispatch(repository_root, set_upstream ? "Publishing branch…" : "Pushing…", success,
                  [repository_root, branch = std::move(branch), set_upstream]() {
                    return project::RunGitRemoteOperation(
                        repository_root, project::GitRemoteOperationKind::Push, branch,
                        set_upstream);
                  });
}

bool GitOperationService::Sync(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Syncing…", "Synced with remote", [repository_root]() {
    project::GitOperationReport pull = project::RunGitRemoteOperation(
        repository_root, project::GitRemoteOperationKind::Pull);
    if (!pull.success()) {
      return pull;
    }
    project::GitOperationReport push = project::RunGitRemoteOperation(
        repository_root, project::GitRemoteOperationKind::Push);
    // Keep both halves' output so the panel shows what the pull did even when the
    // push is what failed.
    if (!pull.output.empty()) {
      if (!push.output.empty()) {
        push.output.insert(0, pull.output + "\n");
      } else {
        push.output = pull.output;
      }
    }
    return push;
  });
}

bool GitOperationService::Stash(const std::filesystem::path& repository_root,
                                std::string message,
                                const bool include_untracked) {
  return Dispatch(repository_root, "Stashing…", "Stashed local changes",
                  [repository_root, message = std::move(message), include_untracked]() {
                    return project::StashGitChanges(repository_root, message, include_untracked);
                  });
}

bool GitOperationService::StashPop(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Popping stash…", "Restored stashed changes",
                  [repository_root]() { return project::PopGitStash(repository_root); });
}

}  // namespace microide::workspace
