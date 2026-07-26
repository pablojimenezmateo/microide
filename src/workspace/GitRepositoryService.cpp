#include "workspace/GitRepositoryService.h"

#include <SDL3/SDL_timer.h>

#include <unordered_set>
#include <utility>

#include "workspace/GitSidebarCommandCenter.h"
#include "project/GitCompareService.h"
#include "project/GitPorcelainV2Parser.h"
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

}  // namespace

GitRepositoryService::GitRepositoryService(project::ProjectBackgroundExecutor& background_executor)
    : background_executor_(background_executor) {}

void GitRepositoryService::SetWakeCallbacks(WakeCallbacks callbacks) {
  wake_callbacks_ = std::move(callbacks);
}

void GitRepositoryService::Reset() {
  // TD-2026-07-17-093: do NOT cancel the shared project background executor here.
  // That queue is also used by commit workflow, patch apply, project-file-monitor
  // arming, project-state refresh, and raster decode; a git-state reset must not
  // discard their queued work. Git-result correctness is already guaranteed by
  // generation gating — every posted refresh carries `request.generation`, and its
  // completion discards itself when `request.generation != refresh_generation_`.
  // Bumping the generation below (via the reset to 0; subsequent refreshes do
  // `++refresh_generation_`) supersedes any in-flight git refresh. The lifecycle
  // reset path (WorkspaceShell::ResetProjectScopedState) still cancels the shared
  // executor itself, before calling this — that is the correct owner of queue-wide
  // cancellation, not this leaf git service.
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

bool GitRepositoryService::IsRefreshing() const {
  std::lock_guard lock(mutex_);
  return refresh_in_flight_ || current_state_.refreshing;
}

void GitRepositoryService::MarkStale() {
  std::lock_guard lock(mutex_);
  current_state_.stale = true;
}

bool GitRepositoryService::IsGitRepoValid(const std::filesystem::path& project_root) {
  return project::GitRepository(project_root).IsValid();
}

void GitRepositoryService::SetRepositoryStateProviderForTesting(
    RepositoryStateProviderForTesting provider) {
  repository_state_provider_for_testing_ = std::move(provider);
}

project::GitRepositoryState GitRepositoryService::BuildRepositoryState(
    const RefreshRequest& request) const {
  // Test seam: substitute the injected producer for the `git status` subprocess.
  // The provider is set-once on the main thread before any refresh is dispatched;
  // this read runs on the worker (or the synchronous test path) with the executor
  // queue's mutex providing the happens-before edge. Stamp the request generation
  // so downstream staleness checks stay authoritative regardless of what the fake
  // returns.
  if (repository_state_provider_for_testing_) {
    project::GitRepositoryState state =
        repository_state_provider_for_testing_(request.project_root, request.generation);
    state.repository_root = request.project_root;
    state.generation = request.generation;
    state.refreshed_at_ms = SDL_GetTicks();
    state.refreshing = false;
    return state;
  }

  project::GitRepositoryState state{
      .repository_root = request.project_root,
      .branch = {},
      .entries = {},
      .tree_git_statuses = {},
      .refresh_error = {},
      .generation = request.generation,
      .refreshed_at_ms = SDL_GetTicks(),
      .refreshing = true,
  };

  const project::GitRepository repo(request.project_root);
  if (!repo.IsValid()) {
    state.repo_available = false;
    state.refresh_error = {
        .category = project::GitRefreshErrorCategory::NotARepo,
        .detail = "not a git repository",
    };
    state.refreshing = false;
    return state;
  }

  const auto result = repo.Execute(
      {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"},
      false);
  if (!result.success()) {
    state.repo_available = true;
    state.refresh_error = {
        .category = project::ClassifyGitRefreshFailure(result.exit_code, result.output),
        .detail = result.output,
    };
    state.stale = true;
    state.refreshing = false;
    return state;
  }

  state = project::GitPorcelainV2Parser::Parse(result.output, request.project_root,
                                               request.generation, state.refreshed_at_ms);
  state.repo_available = true;
  // Cheap filesystem probes on the same background refresh — porcelain v2 does
  // not report the in-flight operation, and nothing else wrote this field, so it
  // stayed None forever and the merge resolver's rebase/cherry-pick label was
  // unreachable.
  state.operation_state = project::DetectGitOperationState(request.project_root);
  state.refreshing = false;
  return state;
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
  snapshot.snapshot_stale = repository_state.stale;
  snapshot.refresh_error = repository_state.refresh_error.detail;
  snapshot.base_ref = resolved_base.base_ref;
  snapshot.base_label = resolved_base.base_label;

  if (include_outgoing_entries && !snapshot.base_ref.empty()) {
    std::unordered_set<std::string> conflicted_paths;
    conflicted_paths.reserve(snapshot.entries.size());
    for (const GitSidebarState::RefreshSnapshotEntry& entry : snapshot.entries) {
      if (entry.conflicted) {
        conflicted_paths.insert(entry.relative_path.generic_string());
      }
    }

    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(request.project_root, snapshot.base_ref);
    for (const auto& entry : outgoing_entries) {
      const std::string path_key = entry.relative_path.generic_string();
      if (conflicted_paths.contains(path_key)) {
        continue;
      }
      snapshot.entries.push_back(GitSidebarState::RefreshSnapshotEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .relative_path = entry.relative_path,
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
      // Superseded between the pre-publish generation re-check and here. Route
      // through the shared superseded-exit path (below, outside the lock) so
      // refresh_in_flight_ is cleared, the background-task counter is balanced,
      // and any deferred follow-up still runs. Returning here directly (the old
      // behavior) leaked the counter and froze the refresh state machine.
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
  // The global background-task counter is owned by ProjectBackgroundExecutor's
  // queue hooks (on_enqueue/on_complete), which balance it exactly once per job
  // on every exit path — run, cancel-skip, PostLatest dedup-drop, shutdown-drain.
  // The service adds no manual counting of its own (it would double-count).
  background_executor_.PostLatest(
      "git-repository-refresh",
      [this, request = std::move(request)]() mutable {
        project::GitRepositoryState repository_state = BuildRepositoryState(request);
        bool superseded = false;
        {
          std::lock_guard lock(mutex_);
          if (request.generation != refresh_generation_) {
            superseded = true;
          } else {
            current_state_ = repository_state;
          }
        }
        if (superseded) {
          HandleSupersededRefresh();
          return;
        }
        // Best-effort early-out: skip building a snapshot that a newer refresh
        // has already superseded. Read the generation under the lock — every
        // other access is mutex-guarded, so an unlocked read here is a data race
        // (PublishSnapshot still re-checks authoritatively under the lock).
        bool generation_current = false;
        {
          std::lock_guard lock(mutex_);
          generation_current = request.generation == refresh_generation_;
        }
        if (!generation_current) {
          HandleSupersededRefresh();
          return;
        }

        GitSidebarState::RefreshSnapshot snapshot =
            BuildSidebarSnapshot(repository_state, request);
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
  ResolvedGitOutgoingBase resolved = ResolveGitOutgoingBase(
      request.project_root, request.outgoing_base_choice, repository_state.repo_available);
  outgoing_base_cache_key_ = std::move(key);
  outgoing_base_cache_value_ = resolved;
  outgoing_base_cache_valid_ = true;
  return resolved;
}

void GitRepositoryService::RequestRefresh(const std::filesystem::path& project_root,
                                          GitSidebarRefreshScope scope,
                                          OutgoingBaseChoice outgoing_base_choice,
                                          bool tree_git_badges_materialized) {
  if (project_root.empty()) {
    Reset();
    return;
  }

  RefreshRequest request{
      .project_root = project_root,
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
    GitSidebarRefreshScope scope,
    OutgoingBaseChoice outgoing_base_choice,
    bool tree_git_badges_materialized) {
  if (project_root.empty()) {
    Reset();
    return;
  }

  RefreshRequest request{
      .project_root = project_root,
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

  // The outgoing-base memo (below) is lock-free only because BuildSidebarSnapshot is
  // confined to the single background-executor worker. This synchronous test path runs
  // BuildSidebarSnapshot on the CALLING thread, so it would race a still-in-flight
  // async refresh's BuildSidebarSnapshot on the worker. Drain the executor first (with
  // mutex_ NOT held — the worker takes mutex_ to finish) so no worker job is touching
  // the memo/current_state_ while we build synchronously. The follow-up state was
  // cleared under the lock above, so a completing worker won't re-post a job.
  background_executor_.Drain();

  const project::GitRepositoryState repository_state = BuildRepositoryState(request);
  {
    std::lock_guard lock(mutex_);
    current_state_ = repository_state;
  }
  GitSidebarState::RefreshSnapshot snapshot = BuildSidebarSnapshot(repository_state, request);
  // This synchronous test path bypasses ProjectBackgroundExecutor entirely, so it
  // is counter-neutral: it neither enqueues a job nor manually touches the global
  // background-task counter.
  PublishSnapshot(std::move(snapshot), request.generation);
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

}  // namespace microide::workspace
