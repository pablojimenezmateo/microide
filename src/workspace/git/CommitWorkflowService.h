#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "project/CommitWorkflowTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "util/MainThreadMailbox.h"
#include "workspace/git/CommitWorkflowPersistence.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/services/NotificationService.h"

namespace microide::workspace {

class GitRepositoryService;

class CommitWorkflowService {
 public:
  struct Callbacks {
    std::function<void()> request_git_refresh;
    std::function<void(std::string_view channel_id, std::string_view label, std::string line)>
        append_output;
    std::function<void(std::string_view)> show_output_panel;
    std::function<void(std::string_view)> set_command_feedback;
    // Post a transient toast for the commit outcome (always-visible feedback,
    // unlike set_command_feedback which only shows in the command panel).
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> persist_commit_draft;
    std::function<void()> clear_persisted_commit_draft;
    std::function<std::optional<PersistedCommitDraftState>()> load_persisted_commit_draft;
    std::function<void(const std::filesystem::path& path, std::string_view left_ref,
                       std::string_view left_label)>
        open_staged_diff;
    std::function<void(project::CommitOperationKind)> open_commit_confirmation;
    // Ask the user to acknowledge the non-blocking pre-check warnings listed in the
    // argument. Accepting routes back through ConfirmPendingOperation, which records
    // the acknowledgements and dispatches the parked operation.
    std::function<void(std::string)> open_commit_warning_confirmation;
    std::function<void()> request_commit_workflow_redraw;
  };

  explicit CommitWorkflowService(project::ProjectBackgroundExecutor& background_executor,
                                 GitRepositoryService& git_repository_service);

  void SetCallbacks(Callbacks callbacks);

  // The commit runs on a background thread; its result must be published to the
  // shared CommitWorkflowState on the main (render) thread. SetCompletionWakeEvent
  // wires the mailbox that carries the completion closure back, and
  // DrainCompletions runs any queued completion on the main thread. The shell
  // reuses the git-sidebar wake event (a successful commit refreshes git anyway)
  // and drains here before rebuilding the sidebar.
  void SetCompletionWakeEvent(std::uint32_t event_type);
  void DrainCompletions();

  // Test seam: number of commit completions queued for the main thread but not
  // yet drained (lets a test wait for the worker to finish without cancelling it).
  int PendingCompletionCount() const { return completion_mailbox_.PendingCount(); }

  void Open(CommitWorkflowState& state);
  void Close(CommitWorkflowState& state);
  // `run_blocking_conflict_scan` gates the full `git diff --cached` conflict-marker
  // scan. Interactive refreshes (open, per-keystroke draft edits) pass false to keep
  // the shell thread responsive; only the pre-dispatch refresh in RequestCommit pays
  // for the unbounded scan, which still blocks the commit if it fires.
  void RefreshDerivedState(CommitWorkflowState& state, bool run_blocking_conflict_scan = false);
  void OnDraftEdited(CommitWorkflowState& state);
  bool RequestCommit(CommitWorkflowState& state, project::CommitOperationKind operation);
  bool ConfirmPendingOperation(CommitWorkflowState& state);
  void CancelPendingConfirmation(CommitWorkflowState& state);
  bool CanExecuteCommit(const CommitWorkflowState& state) const;
  void RestorePersistedDraft(CommitWorkflowState& state,
                             const PersistedCommitDraftState& persisted);

  // Invalidates the in-flight operation identified by `generation`, so its queued
  // completion is dropped instead of publishing into a destroyed state. Called from
  // ~CommitWorkflowState; a no-op if that operation was already superseded or
  // published. The generation guard in the completion already handled the
  // "a newer commit was dispatched" case — this covers the "the target went away"
  // case, which is a project tab closing while its commit runs.
  void AbandonOperation(std::uint64_t generation);

  // A successful commit marks the repository stale and asks for a refresh, then
  // reports plain success — the refresh runs long after and its outcome used to
  // reach the user only as the git sidebar's "Git refresh failed: …" banner, so
  // `CommitOperationResultCategory::RefreshFailedAfterSuccess` had two live
  // display branches and no producer (TD-2026-07-26-003). The shell calls this
  // with every consumed refresh snapshot; when the snapshot is the one a commit
  // was waiting on and it failed, a follow-up warning toast is posted.
  //
  // Follow-up, not replacement: the commit really did succeed and saying so
  // promptly matters more than waiting out a refresh that may take seconds. A
  // newer commit disarms the watch, so the operation in front of the user always
  // owns the feedback.
  void NoteGitRefreshOutcome(std::uint64_t snapshot_generation,
                             std::string_view refresh_error);

 private:
  void DispatchCommit(CommitWorkflowState& state, project::CommitOperationKind operation);
  void PublishResult(CommitWorkflowState& state, project::CommitOperationResult result,
                     project::CommitOperationKind operation, std::uint64_t repository_generation);
  project::CommitDraftContext CurrentDraftContext() const;

  project::ProjectBackgroundExecutor& background_executor_;
  GitRepositoryService& git_repository_service_;
  Callbacks callbacks_;
  mutable std::mutex mutex_;
  std::uint64_t operation_generation_ = 0;
  // 0 = not watching. Otherwise the lowest refresh generation whose snapshot can
  // be the post-commit refresh (see NoteGitRefreshOutcome). It is "current + 1"
  // rather than the generation the request produced because the request can be
  // throttled or folded into an already-deferred refresh, in which case the
  // commit's changes are picked up by a later generation than the call returned.
  std::uint64_t post_commit_refresh_watch_ = 0;
  // Background-thread commit result -> main-thread PublishResult marshaling. Keeps
  // all mutation of CommitWorkflowState (subject/body viewports, status message)
  // on the render thread; the worker only produces the CommitOperationResult.
  util::MainThreadMailbox completion_mailbox_;
};

}  // namespace microide::workspace
