#include "workspace/git/GitRepositoryService.h"

#include "project/GitMetadataSource.h"

#include <SDL3/SDL_timer.h>

#include <unordered_set>
#include <utility>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "project/GitCommandUtil.h"
#include "project/GitCompareService.h"
#include "project/GitPorcelainV2Parser.h"
#include "project/GitStatusRefresh.h"
#include "project/GitRepository.h"
#include "project/GitRepositoryState.h"
#include "project/GitStatusService.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

std::string BranchLabelFromState(const project::GitRepositoryState& state) {
  if (!state.repo_available) {
    return {};
  }
  switch (state.branch.head_kind) {
    case project::GitHeadKind::Detached:
      if (!state.branch.branch_name.empty()) {
        return "detached @ " + state.branch.branch_name;
      }
      if (!state.branch.head_oid.empty()) {
        return "detached @ " + state.branch.head_oid.substr(0, 7);
      }
      return "detached";
    case project::GitHeadKind::Unborn:
      return "(unborn)";
    case project::GitHeadKind::Normal:
      if (!state.branch.branch_name.empty()) {
        return state.branch.branch_name;
      }
      break;
  }
  if (!state.branch.head_oid.empty()) {
    return state.branch.head_oid.substr(0, 7);
  }
  return "HEAD";
}

}

GitRepositoryService::GitRepositoryService(project::ProjectBackgroundExecutor& background_executor)
    : background_executor_(background_executor) {}

void GitRepositoryService::SetWakeCallbacks(WakeCallbacks callbacks) {
  wake_callbacks_ = std::move(callbacks);
}

void GitRepositoryService::Reset() {
  std::lock_guard lock(mutex_);
  current_state_ = {};
  pending_sidebar_snapshot_.reset();
  refresh_generation_ = 0;
  refresh_in_flight_ = false;
  follow_up_refresh_pending_ = false;
  deferred_refresh_.reset();
  active_project_root_.clear();
}

project::GitRepositoryState GitRepositoryService::CurrentState() const {
  std::lock_guard lock(mutex_);
  return current_state_;
}

GitRepositoryService::Summary GitRepositoryService::CurrentSummary() const {
  std::lock_guard lock(mutex_);
  Summary summary;
  summary.generation = current_state_.generation;
  summary.repo_available = current_state_.repo_available;
  summary.stale = current_state_.stale;
  for (const project::GitRepositoryEntry& entry : current_state_.entries) {
    if (entry.conflicted) {
      ++summary.conflicted_entry_count;
    }
  }
  return summary;
}

bool GitRepositoryService::IsRefreshing() const {
  std::lock_guard lock(mutex_);
  return refresh_in_flight_ || current_state_.refreshing;
}

void GitRepositoryService::MarkStale() {
  std::lock_guard lock(mutex_);
  current_state_.stale = true;
}

bool GitRepositoryService::IsGitRepoValid(const std::filesystem::path& project_root,
                                          const platform::ProcessLauncher& launcher) {
  return project::GitMetadataFor(launcher).IsRepository(project_root);
}

void GitRepositoryService::SetRepositoryStateProviderForTesting(
    RepositoryStateProviderForTesting provider) {
  repository_state_provider_for_testing_ = std::move(provider);
}

project::GitRepositoryState GitRepositoryService::BuildRepositoryState(
    const RefreshRequest& request) const {
  if (repository_state_provider_for_testing_) {
    project::GitRepositoryState state =
        repository_state_provider_for_testing_(request.project_root, request.generation);
    state.repository_root = request.project_root;
    state.generation = request.generation;
    state.refreshed_at_ms = SDL_GetTicks();
    state.refreshing = false;
    return state;
  }

  const project::GitRepository repo(request.project_root, *request.launcher);
  return project::BuildGitRepositoryStateFromStatus(repo, request.project_root,
                                                    request.generation, SDL_GetTicks());
}

GitSidebarState::RefreshSnapshot GitRepositoryService::BuildSidebarSnapshot(
    const project::GitRepositoryState& repository_state,
    const RefreshRequest& request) const {
  GitSidebarState::RefreshSnapshot snapshot;
  snapshot.generation = repository_state.generation;

  const bool materialize_tree_git_badges =
      request.scope == GitSidebarRefreshScope::TreeBadges ||
      (request.scope == GitSidebarRefreshScope::Full && request.tree_git_badges_materialized) ||
      (request.scope == GitSidebarRefreshScope::StatusOnly &&
       request.tree_git_badges_materialized);
  const bool include_outgoing_entries = request.scope == GitSidebarRefreshScope::Full;
  const bool populate_sidebar_entries = request.scope != GitSidebarRefreshScope::TreeBadges;

  if (materialize_tree_git_badges) {
    snapshot.includes_tree_git_statuses = true;
    snapshot.tree_git_statuses = repository_state.tree_git_statuses;
  }

  if (!populate_sidebar_entries) {
    return snapshot;
  }

  for (const project::GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.kind == project::GitRepositoryEntryKind::Ignored) {
      continue;
    }
    snapshot.entries.push_back(GitSidebarState::RefreshSnapshotEntry{
        .section = ClassifyGitSidebarSection(entry.conflicted, entry.staged, entry.status),
        .relative_path = entry.path.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
        .is_staged_rename = entry.staged && entry.old_path.has_value(),
    });
  }

  const ResolvedGitOutgoingBase resolved_base =
      ResolveOutgoingBaseCached(repository_state, request);
  snapshot.repo_available = resolved_base.repo_available;
  snapshot.branch_label = BranchLabelFromState(repository_state);
  snapshot.upstream_label = repository_state.branch.upstream;
  snapshot.ahead = repository_state.branch.ahead;
  snapshot.behind = repository_state.branch.behind;
  snapshot.worktree_count = repository_state.worktree_count;
  snapshot.snapshot_stale = repository_state.stale;
  snapshot.refresh_error = repository_state.refresh_error.detail;
  snapshot.base_ref = resolved_base.base_ref;
  snapshot.base_label = resolved_base.base_label;

  if (include_outgoing_entries && !snapshot.base_ref.empty()) {
    std::unordered_set<std::string> conflicted_paths;
    conflicted_paths.reserve(snapshot.entries.size());
    for (const GitSidebarState::RefreshSnapshotEntry& entry : snapshot.entries) {
      if (entry.conflicted) {
        conflicted_paths.insert(entry.relative_path);
      }
    }

    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(request.project_root, *request.launcher,
                                               snapshot.base_ref);
    for (const auto& entry : outgoing_entries) {
      std::string path_key = entry.relative_path.generic_string();
      if (conflicted_paths.contains(path_key)) {
        continue;
      }
      snapshot.entries.push_back(GitSidebarState::RefreshSnapshotEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .relative_path = std::move(path_key),
          .status = entry.status,
          .conflicted = false,
          .staged = false,
      });
    }
  }

  return snapshot;
}

void GitRepositoryService::HandleSupersededRefresh() {
  std::optional<RefreshRequest> deferred_follow_up;
  {
    std::lock_guard lock(mutex_);
    refresh_in_flight_ = false;
    if (follow_up_refresh_pending_ && deferred_refresh_.has_value()) {
      deferred_follow_up = *deferred_refresh_;
      deferred_refresh_.reset();
      follow_up_refresh_pending_ = false;
      deferred_follow_up->generation = ++refresh_generation_;
      refresh_in_flight_ = true;
      current_state_.refreshing = true;
    }
  }
  if (deferred_follow_up.has_value()) {
    ScheduleRefresh(std::move(*deferred_follow_up));
  }
}

void GitRepositoryService::PublishSnapshot(GitSidebarState::RefreshSnapshot snapshot,
                                           std::uint64_t generation) {
  bool superseded = false;
  bool needs_follow_up = false;
  {
    std::lock_guard lock(mutex_);
    if (generation != refresh_generation_) {
      superseded = true;
    } else {
      pending_sidebar_snapshot_ = std::move(snapshot);
      current_state_.stale = false;
      current_state_.refreshing = false;
      refresh_in_flight_ = false;
      if (follow_up_refresh_pending_ && deferred_refresh_.has_value()) {
        needs_follow_up = true;
        follow_up_refresh_pending_ = false;
      }
    }
  }

  if (superseded) {
    HandleSupersededRefresh();
    return;
  }

  if (wake_callbacks_.push_refresh_ready_event != nullptr) {
    (void)wake_callbacks_.push_refresh_ready_event();
  }

  if (needs_follow_up) {
    RefreshRequest request;
    {
      std::lock_guard lock(mutex_);
      request = *deferred_refresh_;
      deferred_refresh_.reset();
      request.generation = ++refresh_generation_;
      refresh_in_flight_ = true;
      current_state_.stale = true;
      current_state_.refreshing = true;
    }
    ScheduleRefresh(std::move(request));
  }
}

void GitRepositoryService::ScheduleRefresh(RefreshRequest request) {
  background_executor_.PostLatest(
      "git-repository-refresh",
      [this, request = std::move(request)]() mutable {
        project::GitRepositoryState repository_state = BuildRepositoryState(request);
        bool superseded = false;
        {
          std::lock_guard lock(mutex_);
          superseded = request.generation != refresh_generation_;
        }
        if (superseded) {
          HandleSupersededRefresh();
          return;
        }

        GitSidebarState::RefreshSnapshot snapshot =
            BuildSidebarSnapshot(repository_state, request);
        {
          std::lock_guard lock(mutex_);
          if (request.generation != refresh_generation_) {
            superseded = true;
          } else {
            current_state_ = std::move(repository_state);
          }
        }
        if (superseded) {
          HandleSupersededRefresh();
          return;
        }
        PublishSnapshot(std::move(snapshot), request.generation);
      });
}

ResolvedGitOutgoingBase GitRepositoryService::ResolveOutgoingBaseCached(
    const project::GitRepositoryState& repository_state, const RefreshRequest& request) const {
  OutgoingBaseCacheKey key{
      .root = request.project_root,
      .choice_kind = request.outgoing_base_choice.kind,
      .custom_ref = request.outgoing_base_choice.custom_ref,
      .head_oid = repository_state.branch.head_oid,
      .branch_name = repository_state.branch.branch_name,
      .upstream = repository_state.branch.upstream,
      .repo_available = repository_state.repo_available,
  };
  if (outgoing_base_cache_valid_ && outgoing_base_cache_key_ == key) {
    return outgoing_base_cache_value_;
  }
  ++outgoing_base_resolve_count_;
  ResolvedGitOutgoingBase resolved =
      ResolveGitOutgoingBase(request.project_root, *request.launcher,
                             request.outgoing_base_choice, repository_state.repo_available);
  outgoing_base_cache_key_ = std::move(key);
  outgoing_base_cache_value_ = resolved;
  outgoing_base_cache_valid_ = true;
  return resolved;
}

void GitRepositoryService::RequestRefresh(const std::filesystem::path& project_root,
                                          const platform::ProcessLauncher& launcher,
                                          GitSidebarRefreshScope scope,
                                          OutgoingBaseChoice outgoing_base_choice,
                                          bool tree_git_badges_materialized) {
  if (project_root.empty()) {
    Reset();
    return;
  }

  RefreshRequest request{
      .project_root = project_root,
      .launcher = &launcher,
      .scope = scope,
      .outgoing_base_choice = outgoing_base_choice,
      .tree_git_badges_materialized = tree_git_badges_materialized,
  };

  bool schedule_now = true;
  {
    std::lock_guard lock(mutex_);
    active_project_root_ = project_root;
    if (refresh_in_flight_) {
      follow_up_refresh_pending_ = true;
      deferred_refresh_ = request;
      current_state_.stale = true;
      schedule_now = false;
    } else {
      refresh_in_flight_ = true;
      current_state_.stale = true;
      current_state_.refreshing = true;
    }
    request.generation = ++refresh_generation_;
  }

  if (schedule_now) {
    ScheduleRefresh(std::move(request));
  }
}

void GitRepositoryService::RunRefreshSynchronouslyForTesting(
    const std::filesystem::path& project_root,
    const platform::ProcessLauncher& launcher,
    GitSidebarRefreshScope scope,
    OutgoingBaseChoice outgoing_base_choice,
    bool tree_git_badges_materialized) {
  if (project_root.empty()) {
    Reset();
    return;
  }

  RefreshRequest request{
      .project_root = project_root,
      .launcher = &launcher,
      .scope = scope,
      .outgoing_base_choice = outgoing_base_choice,
      .tree_git_badges_materialized = tree_git_badges_materialized,
  };
  {
    std::lock_guard lock(mutex_);
    active_project_root_ = project_root;
    refresh_in_flight_ = false;
    follow_up_refresh_pending_ = false;
    deferred_refresh_.reset();
    request.generation = ++refresh_generation_;
    current_state_.stale = true;
    current_state_.refreshing = true;
  }

  background_executor_.Drain();

  project::GitRepositoryState repository_state = BuildRepositoryState(request);
  GitSidebarState::RefreshSnapshot snapshot = BuildSidebarSnapshot(repository_state, request);
  {
    std::lock_guard lock(mutex_);
    current_state_ = std::move(repository_state);
  }
  PublishSnapshot(std::move(snapshot), request.generation);
}

std::uint64_t GitRepositoryService::CurrentRefreshGeneration() const {
  std::lock_guard lock(mutex_);
  return refresh_generation_;
}

bool GitRepositoryService::ConsumePendingSidebarSnapshot(
    GitSidebarState::RefreshSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return false;
  }

  std::optional<GitSidebarState::RefreshSnapshot> pending;
  {
    std::lock_guard lock(mutex_);
    pending = std::move(pending_sidebar_snapshot_);
    pending_sidebar_snapshot_.reset();
    if (pending.has_value()) {
      current_state_.refreshing = false;
    }
  }
  if (!pending.has_value()) {
    return false;
  }

  *snapshot = std::move(*pending);
  return true;
}

}
