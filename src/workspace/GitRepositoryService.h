#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

#include "project/GitRepositoryState.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/WorkspaceGitOutgoingBase.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

class GitRepositoryService {
 public:
  struct WakeCallbacks {
    std::function<void()> increment_background_task_count;
    std::function<void()> decrement_background_task_count_and_wake;
    std::function<bool()> push_refresh_ready_event;
  };

  explicit GitRepositoryService(project::ProjectBackgroundExecutor& background_executor);

  void SetWakeCallbacks(WakeCallbacks callbacks);
  void Reset();

  project::GitRepositoryState CurrentState() const;
  bool IsRefreshing() const;

  void MarkStale();
  void RequestRefresh(const std::filesystem::path& project_root,
                      GitSidebarRefreshScope scope,
                      OutgoingBaseChoice outgoing_base_choice,
                      bool tree_git_badges_materialized);
  bool ConsumePendingSidebarSnapshot(GitSidebarState::RefreshSnapshot* snapshot);

  // Test seam: drives a refresh on the calling thread. Always compiled; unused
  // by production code paths (which dispatch refreshes asynchronously).
  void RunRefreshSynchronouslyForTesting(const std::filesystem::path& project_root,
                                         GitSidebarRefreshScope scope,
                                         OutgoingBaseChoice outgoing_base_choice,
                                         bool tree_git_badges_materialized);

  static bool IsGitRepoValid(const std::filesystem::path& project_root);

 private:
  struct RefreshRequest {
    std::filesystem::path project_root;
    GitSidebarRefreshScope scope = GitSidebarRefreshScope::Full;
    OutgoingBaseChoice outgoing_base_choice;
    bool tree_git_badges_materialized = false;
    std::uint64_t generation = 0;
  };

  project::GitRepositoryState BuildRepositoryState(const RefreshRequest& request) const;
  GitSidebarState::RefreshSnapshot BuildSidebarSnapshot(
      const project::GitRepositoryState& repository_state,
      const RefreshRequest& request) const;
  void PublishSnapshot(GitSidebarState::RefreshSnapshot snapshot, std::uint64_t generation);
  void ScheduleRefresh(RefreshRequest request);
  // Shared exit for a refresh task whose generation was superseded before it
  // could publish. Clears refresh_in_flight_, decrements the background-task
  // counter for the finishing task, and re-schedules a deferred follow-up if
  // one is pending. Must be called with mutex_ NOT held.
  void HandleSupersededRefresh();

  project::ProjectBackgroundExecutor& background_executor_;
  WakeCallbacks wake_callbacks_;
  mutable std::mutex mutex_;
  project::GitRepositoryState current_state_;
  std::optional<GitSidebarState::RefreshSnapshot> pending_sidebar_snapshot_;
  std::uint64_t refresh_generation_ = 0;
  bool refresh_in_flight_ = false;
  bool follow_up_refresh_pending_ = false;
  std::optional<RefreshRequest> deferred_refresh_;
  std::filesystem::path active_project_root_;
};

}  // namespace microide::workspace
