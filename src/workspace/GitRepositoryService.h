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

  // Test seam: replaces the `git status` subprocess that BuildRepositoryState
  // would spawn on the refresh worker with an injected state producer. Lets tests
  // block the git query (and assert the sidebar is refreshing before it returns)
  // without running real git — the fake-git seam mirror of the async compare
  // picker's provider seam. Default (empty) runs the real subprocess. Set on the
  // main thread before dispatching refreshes; read on the worker with the queue's
  // enqueue/dequeue mutex as the happens-before edge.
  using RepositoryStateProviderForTesting = std::function<project::GitRepositoryState(
      const std::filesystem::path& project_root, std::uint64_t generation)>;
  void SetRepositoryStateProviderForTesting(RepositoryStateProviderForTesting provider);

  // Count of actual (cache-miss) outgoing-base resolutions performed. A refresh
  // whose repository identity is unchanged serves the base from cache and does
  // not increment this. Test-only observability for the outgoing-base memo.
  std::uint64_t OutgoingBaseResolveCountForTesting() const {
    return outgoing_base_resolve_count_;
  }

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
  // Resolves the outgoing-base ref, memoizing the result across refreshes. The
  // Auto resolution runs several git subprocesses (symbolic-ref, config, show-ref,
  // rev-parse); a plain status refresh after a file edit leaves the identity below
  // unchanged, so it is served from the cache and spawns no git process. Called
  // only from BuildSidebarSnapshot, which the executor serializes.
  ResolvedGitOutgoingBase ResolveOutgoingBaseCached(
      const project::GitRepositoryState& repository_state, const RefreshRequest& request) const;
  void PublishSnapshot(GitSidebarState::RefreshSnapshot snapshot, std::uint64_t generation);
  void ScheduleRefresh(RefreshRequest request);
  // Shared exit for a refresh task whose generation was superseded before it
  // could publish. Clears refresh_in_flight_ and re-schedules a deferred
  // follow-up if one is pending. Must be called with mutex_ NOT held. (The
  // global background-task counter is owned entirely by ProjectBackgroundExecutor's
  // queue hooks, which balance it once per job on every exit path.)
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

  // Outgoing-base memo (see ResolveOutgoingBaseCached). Confined to a single thread
  // via BuildSidebarSnapshot, so it needs no separate lock: production refreshes run
  // it only on the serial background-executor worker, and the synchronous test seam
  // (RunRefreshSynchronouslyForTesting) drains that worker before building on the
  // calling thread, so the two never touch the memo concurrently. The key includes
  // head_oid, so any HEAD movement (commit/checkout/reset/merge) or branch/upstream/
  // root/choice change re-resolves; a file-edit status refresh hits the cache.
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

}  // namespace microide::workspace
