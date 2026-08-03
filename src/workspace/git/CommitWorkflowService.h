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
  // Background-thread commit result -> main-thread PublishResult marshaling. Keeps
  // all mutation of CommitWorkflowState (subject/body viewports, status message)
  // on the render thread; the worker only produces the CommitOperationResult.
  util::MainThreadMailbox completion_mailbox_;
};

}  // namespace microide::workspace
