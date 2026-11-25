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
#include "workspace/services/NotificationService.h"

namespace microide::workspace {

class GitOperationService {
 public:
  struct Callbacks {
    std::function<const platform::ProcessLauncher*()> project_launcher;
    std::function<void()> request_git_refresh;
    std::function<void()> reload_open_buffers;
    std::function<void(std::string_view channel_id, std::string_view label, std::string line)>
        append_output;
    std::function<void(std::string_view)> show_output_panel;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> request_frame;
    std::function<bool(const std::filesystem::path& repository_root)> is_in_front;
    std::function<std::string(const std::filesystem::path& repository_root)> project_label;
    std::function<void(const std::filesystem::path& repository_root)> defer_to_project;
  };

  explicit GitOperationService(project::ProjectBackgroundExecutor& background_executor);

  void SetCallbacks(Callbacks callbacks);
  void SetCompletionWakeEvent(std::uint32_t event_type);
  void DrainCompletions();

  int PendingCompletionCount() const { return completion_mailbox_.PendingCount(); }

  bool busy() const { return in_flight_.load(std::memory_order_acquire); }
  const platform::ProcessLauncher& ProjectLauncher() const;
  const std::string& status_message() const { return status_message_; }

  bool SwitchBranch(const std::filesystem::path& repository_root, std::string branch);
  bool CreateBranch(const std::filesystem::path& repository_root,
                    std::string branch,
                    std::string start_point = {});
  bool Fetch(const std::filesystem::path& repository_root);
  bool Pull(const std::filesystem::path& repository_root);
  bool Push(const std::filesystem::path& repository_root, std::string branch, bool set_upstream);
  bool Sync(const std::filesystem::path& repository_root);
  bool Stash(const std::filesystem::path& repository_root,
             std::string message,
             bool include_untracked);
  bool StashPop(const std::filesystem::path& repository_root);
  bool ResolveConflict(const std::filesystem::path& repository_root, std::string relative_path,
                       project::GitConflictSide side, std::function<void()> resolved = {});

 private:
  bool Dispatch(const std::filesystem::path& repository_root,
                std::string label,
                std::string success_message,
                std::function<project::GitOperationReport()> work,
                std::function<void()> on_success = {});
  void Publish(std::uint64_t generation,
               const std::filesystem::path& repository_root,
               std::string success_message,
               project::GitOperationReport report,
               const std::function<void()>& on_success);

  project::ProjectBackgroundExecutor& background_executor_;
  util::MainThreadMailbox completion_mailbox_;
  Callbacks callbacks_;
  mutable std::mutex mutex_;
  std::uint64_t operation_generation_ = 0;
  std::atomic_bool in_flight_{false};
  std::string status_message_;
};

}
