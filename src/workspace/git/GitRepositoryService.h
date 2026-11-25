#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

#include "platform/ProcessLauncher.h"
#include "project/GitRepositoryState.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/git/WorkspaceGitOutgoingBase.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

class GitRepositoryService {
 public:
  struct WakeCallbacks {
    std::function<bool()> push_refresh_ready_event;
  };

  explicit GitRepositoryService(project::ProjectBackgroundExecutor& background_executor);

  void SetWakeCallbacks(WakeCallbacks callbacks);
  void Reset();

  project::GitRepositoryState CurrentState() const;

  struct Summary {
    std::uint64_t generation = 0;
    std::size_t conflicted_entry_count = 0;
    bool repo_available = false;
    bool stale = false;
  };
  Summary CurrentSummary() const;

  bool IsRefreshing() const;

  void MarkStale();
  void RequestRefresh(const std::filesystem::path& project_root,
                      const platform::ProcessLauncher& launcher,
                      GitSidebarRefreshScope scope,
                      OutgoingBaseChoice outgoing_base_choice,
                      bool tree_git_badges_materialized);
  bool ConsumePendingSidebarSnapshot(GitSidebarState::RefreshSnapshot* snapshot);

  std::uint64_t CurrentRefreshGeneration() const;

  void RunRefreshSynchronouslyForTesting(const std::filesystem::path& project_root,
                                         const platform::ProcessLauncher& launcher,
                                         GitSidebarRefreshScope scope,
                                         OutgoingBaseChoice outgoing_base_choice,
                                         bool tree_git_badges_materialized);

  using RepositoryStateProviderForTesting = std::function<project::GitRepositoryState(
      const std::filesystem::path& project_root, std::uint64_t generation)>;
  void SetRepositoryStateProviderForTesting(RepositoryStateProviderForTesting provider);

  std::uint64_t OutgoingBaseResolveCountForTesting() const {
    return outgoing_base_resolve_count_;
  }

  static bool IsGitRepoValid(const std::filesystem::path& project_root,
                             const platform::ProcessLauncher& launcher);

 private:
  struct RefreshRequest {
    std::filesystem::path project_root;
    const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
    GitSidebarRefreshScope scope = GitSidebarRefreshScope::Full;
    OutgoingBaseChoice outgoing_base_choice;
    bool tree_git_badges_materialized = false;
    std::uint64_t generation = 0;
  };

  project::GitRepositoryState BuildRepositoryState(const RefreshRequest& request) const;
  GitSidebarState::RefreshSnapshot BuildSidebarSnapshot(
      const project::GitRepositoryState& repository_state,
      const RefreshRequest& request) const;
  ResolvedGitOutgoingBase ResolveOutgoingBaseCached(
      const project::GitRepositoryState& repository_state, const RefreshRequest& request) const;
  void PublishSnapshot(GitSidebarState::RefreshSnapshot snapshot, std::uint64_t generation);
  void ScheduleRefresh(RefreshRequest request);
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
  RepositoryStateProviderForTesting repository_state_provider_for_testing_;

  struct OutgoingBaseCacheKey {
    std::filesystem::path root;
    OutgoingBaseChoice::Kind choice_kind = OutgoingBaseChoice::Kind::Auto;
    std::string custom_ref;
    std::string head_oid;
    std::string branch_name;
    std::string upstream;
    bool repo_available = false;
    bool operator==(const OutgoingBaseCacheKey&) const = default;
  };
  mutable bool outgoing_base_cache_valid_ = false;
  mutable OutgoingBaseCacheKey outgoing_base_cache_key_;
  mutable ResolvedGitOutgoingBase outgoing_base_cache_value_;
  mutable std::uint64_t outgoing_base_resolve_count_ = 0;
};

}
