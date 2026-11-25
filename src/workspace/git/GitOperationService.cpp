#include "workspace/git/GitOperationService.h"

#include <utility>

namespace microide::workspace {

namespace {

constexpr std::string_view kOutputChannelId = "git.operation";
constexpr std::string_view kOutputChannelLabel = "Git";

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

bool TouchesWorkingTree(const project::GitOperationOutcome outcome) {
  return outcome == project::GitOperationOutcome::Success ||
         outcome == project::GitOperationOutcome::Conflict;
}

}

GitOperationService::GitOperationService(project::ProjectBackgroundExecutor& background_executor)
    : background_executor_(background_executor) {}

void GitOperationService::SetCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void GitOperationService::SetCompletionWakeEvent(const std::uint32_t event_type) {
  completion_mailbox_.SetWakeChannel(event_type);
}

void GitOperationService::DrainCompletions() { completion_mailbox_.Drain(); }

bool GitOperationService::Dispatch(const std::filesystem::path& repository_root,
                                   std::string label,
                                   std::string success_message,
                                   std::function<project::GitOperationReport()> work,
                                   std::function<void()> on_success) {
  if (repository_root.empty()) {
    return false;
  }
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
  if (callbacks_.request_frame != nullptr) {
    callbacks_.request_frame();
  }

  background_executor_.PostDurable([this, generation, repository_root, work = std::move(work),
                             success_message = std::move(success_message),
                             on_success = std::move(on_success)]() mutable {
    project::GitOperationReport report = work();
    completion_mailbox_.Post([this, generation, repository_root = std::move(repository_root),
                              success_message = std::move(success_message), report = std::move(report),
                              on_success = std::move(on_success)]() mutable {
      Publish(generation, repository_root, std::move(success_message), std::move(report), on_success);
    });
  });
  return true;
}

void GitOperationService::Publish(const std::uint64_t generation,
                                  const std::filesystem::path& repository_root,
                                  std::string success_message,
                                  project::GitOperationReport report,
                                  const std::function<void()>& on_success) {
  {
    std::lock_guard lock(mutex_);
    if (generation != operation_generation_) {
      return;
    }
  }
  status_message_.clear();
  in_flight_.store(false, std::memory_order_release);
  const bool in_front = !callbacks_.is_in_front || callbacks_.is_in_front(repository_root);

  if (!report.output.empty() && callbacks_.append_output != nullptr) {
    callbacks_.append_output(kOutputChannelId, kOutputChannelLabel, report.output);
    if (!report.success() && in_front && callbacks_.show_output_panel != nullptr) {
      callbacks_.show_output_panel(kOutputChannelId);
    }
  }

  if (callbacks_.notify != nullptr) {
    std::string message = report.detail.empty() ? std::move(success_message)
                                                : std::move(report.detail);
    if (message.empty()) {
      message = "Git operation finished";
    }
    if (!in_front && callbacks_.project_label) {
      message += " in " + callbacks_.project_label(repository_root);
    }
    callbacks_.notify(ToneFor(report.outcome), std::move(message));
  }

  if (!in_front) {
    if ((report.success() || TouchesWorkingTree(report.outcome)) && callbacks_.defer_to_project) {
      callbacks_.defer_to_project(repository_root);
    }
    if (callbacks_.request_frame != nullptr) {
      callbacks_.request_frame();
    }
    return;
  }
  if (report.outcome == project::GitOperationOutcome::Success && on_success) {
    on_success();
  }
  if (report.success() || TouchesWorkingTree(report.outcome)) {
    if (callbacks_.request_git_refresh != nullptr) {
      callbacks_.request_git_refresh();
    }
    if (callbacks_.reload_open_buffers != nullptr) {
      callbacks_.reload_open_buffers();
    }
  }
  if (callbacks_.request_frame != nullptr) {
    callbacks_.request_frame();
  }
}

const platform::ProcessLauncher& GitOperationService::ProjectLauncher() const {
  return platform::LauncherOrLocal(callbacks_.project_launcher ? callbacks_.project_launcher()
                                                                 : nullptr);
}

bool GitOperationService::SwitchBranch(const std::filesystem::path& repository_root,
                                       std::string branch) {
  if (branch.empty()) {
    return false;
  }
  std::string success = "Switched to " + branch;
  return Dispatch(repository_root, "Switching branch…", std::move(success),
                  [repository_root, launcher = &ProjectLauncher(),
                   branch = std::move(branch)]() {
                    return project::SwitchGitBranch(repository_root, *launcher, branch);
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
                  [repository_root, launcher = &ProjectLauncher(), branch = std::move(branch),
                   start_point = std::move(start_point)]() {
                    return project::CreateGitBranch(repository_root, *launcher, branch,
                                                    start_point);
                  });
}

bool GitOperationService::Fetch(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Fetching…", "Fetched from remote",
                  [repository_root, launcher = &ProjectLauncher()]() {
                    return project::RunGitRemoteOperation(
                        repository_root, *launcher, project::GitRemoteOperationKind::Fetch);
                  });
}

bool GitOperationService::Pull(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Pulling…", "Pulled from remote",
                  [repository_root, launcher = &ProjectLauncher()]() {
                    return project::RunGitRemoteOperation(
                        repository_root, *launcher, project::GitRemoteOperationKind::Pull);
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
                  [repository_root, launcher = &ProjectLauncher(), branch = std::move(branch),
                   set_upstream]() {
                    return project::RunGitRemoteOperation(
                        repository_root, *launcher, project::GitRemoteOperationKind::Push, branch,
                        set_upstream);
                  });
}

bool GitOperationService::Sync(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Syncing…", "Synced with remote",
                  [repository_root, launcher = &ProjectLauncher()]() {
    project::GitOperationReport pull = project::RunGitRemoteOperation(
        repository_root, *launcher, project::GitRemoteOperationKind::Pull);
    if (!pull.success()) {
      return pull;
    }
    project::GitOperationReport push = project::RunGitRemoteOperation(
        repository_root, *launcher, project::GitRemoteOperationKind::Push);
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
                  [repository_root, launcher = &ProjectLauncher(),
                   message = std::move(message), include_untracked]() {
                    return project::StashGitChanges(repository_root, *launcher, message,
                                                    include_untracked);
                  });
}

bool GitOperationService::StashPop(const std::filesystem::path& repository_root) {
  return Dispatch(repository_root, "Popping stash…", "Restored stashed changes",
                  [repository_root, launcher = &ProjectLauncher()]() {
                    return project::PopGitStash(repository_root, *launcher);
                  });
}

bool GitOperationService::ResolveConflict(const std::filesystem::path& repository_root,
                                          std::string relative_path,
                                          const project::GitConflictSide side,
                                          std::function<void()> resolved) {
  const bool current = side == project::GitConflictSide::Current;
  std::string success = (current ? "Kept the local version of " : "Took the remote version of ") +
                        relative_path;
  return Dispatch(repository_root, "Resolving conflict…", std::move(success),
                  [repository_root, launcher = &ProjectLauncher(),
                   relative_path = std::move(relative_path), side]() {
                    return project::ResolveGitConflictWithSide(repository_root, *launcher,
                                                               relative_path, side);
                  },
                  std::move(resolved));
}

}
