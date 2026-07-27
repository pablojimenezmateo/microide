#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "project/GitBranchOperations.h"
#include "project/ProjectBackgroundExecutor.h"
#include "util/MainThreadMailbox.h"
#include "workspace/NotificationService.h"

namespace microide::workspace {

// The write half of the git workstation: branch switch/create, fetch/pull/push,
// and stash push/pop.
//
// Every operation is a subprocess that can block for seconds (network, hooks,
// checkout of a large tree), so none of them may run on the shell thread. This
// service owns that contract: dispatch on the project background executor, hop the
// result back through a main-thread mailbox, and publish feedback (toast + output
// channel + git refresh) exactly once on the render thread. It is the same
// executor/mailbox shape CommitWorkflowService uses, kept deliberately parallel.
//
// Only one operation runs at a time. A second request while one is in flight is
// rejected rather than queued: two concurrent `git switch`es would race the index
// lock and the user would have no idea which won.
class GitOperationService {
 public:
  struct Callbacks {
    // Re-read git state after an operation that could have changed HEAD, the index,
    // or the working tree.
    std::function<void()> request_git_refresh;
    // Reload clean open buffers whose on-disk content the operation replaced (a
    // branch switch rewrites the worktree under the editor).
    std::function<void()> reload_open_buffers;
    std::function<void(std::string_view channel_id, std::string_view label, std::string line)>
        append_output;
    std::function<void(std::string_view)> show_output_panel;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> request_redraw;
  };

  explicit GitOperationService(project::ProjectBackgroundExecutor& background_executor);

  void SetCallbacks(Callbacks callbacks);
  void SetCompletionWakeEvent(std::uint32_t event_type);
  void DrainCompletions();

  // Test seam: completions queued for the main thread but not yet drained.
  int PendingCompletionCount() const { return completion_mailbox_.PendingCount(); }

  bool busy() const { return in_flight_.load(std::memory_order_acquire); }
  // Label of the operation currently running ("Switching branch…"), for the status
  // surface. Empty when idle. Main-thread only.
  const std::string& status_message() const { return status_message_; }

  // Each returns false when the request was rejected outright (no repository, an
  // operation already in flight, or an empty required argument) — i.e. nothing was
  // dispatched and no completion will arrive.
  bool SwitchBranch(const std::filesystem::path& repository_root, std::string branch);
  bool CreateBranch(const std::filesystem::path& repository_root,
                    std::string branch,
                    std::string start_point = {});
  bool Fetch(const std::filesystem::path& repository_root);
  bool Pull(const std::filesystem::path& repository_root);
  // `set_upstream` publishes an unpublished branch (`push --set-upstream origin
  // <branch>`); `branch` is required in that case.
  bool Push(const std::filesystem::path& repository_root, std::string branch, bool set_upstream);
  // Pull then push in one background task, stopping at the first failure. Sync is
  // one user intent ("catch up and publish"), and running it as two dispatches
  // would be rejected by the one-at-a-time guard partway through.
  bool Sync(const std::filesystem::path& repository_root);
  bool Stash(const std::filesystem::path& repository_root,
             std::string message,
             bool include_untracked);
  bool StashPop(const std::filesystem::path& repository_root);

 private:
  // Dispatches `work` on the background executor and publishes its report. `label`
  // is the in-flight status text; `success_message` is the toast on a clean result.
  bool Dispatch(const std::filesystem::path& repository_root,
                std::string label,
                std::string success_message,
                std::function<project::GitOperationReport()> work);
  void Publish(std::uint64_t generation,
               std::string success_message,
               project::GitOperationReport report);

  project::ProjectBackgroundExecutor& background_executor_;
  util::MainThreadMailbox completion_mailbox_;
  Callbacks callbacks_;
  mutable std::mutex mutex_;
  std::uint64_t operation_generation_ = 0;
  // Read from the shell thread every frame (status text), written when a dispatch
  // starts/finishes; atomic so the frame read needs no lock.
  std::atomic_bool in_flight_{false};
  std::string status_message_;
};

}  // namespace microide::workspace
