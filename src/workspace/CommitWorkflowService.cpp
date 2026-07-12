#include "workspace/CommitWorkflowService.h"

#include <utility>

#include "project/CommitWorkflowChecks.h"
#include "project/GitCommitExecutor.h"
#include "workspace/GitRepositoryService.h"

namespace microide::workspace {
namespace {

void SetBodyText(editor::TextViewport& viewport, std::string_view text) {
  viewport.LoadContent(text);
}

std::string ResultFeedback(const project::CommitOperationResult& result) {
  if (!result.detail.empty()) {
    return result.detail;
  }
  switch (result.category) {
    case project::CommitOperationResultCategory::Success:
      return "Commit created";
    case project::CommitOperationResultCategory::HookFailed:
      return "Commit hook failed";
    case project::CommitOperationResultCategory::AuthFailed:
      return "Commit author or signing is not configured";
    case project::CommitOperationResultCategory::RepoLocked:
      return "Git repository is locked";
    case project::CommitOperationResultCategory::Conflict:
      return "Commit failed due to conflicts";
    case project::CommitOperationResultCategory::DirtyWorktree:
      return "Nothing to commit";
    case project::CommitOperationResultCategory::RefreshFailedAfterSuccess:
      return "Commit succeeded, but repository refresh failed";
    default:
      return "Commit failed";
  }
}

NotificationService::Tone ResultTone(project::CommitOperationResultCategory category) {
  switch (category) {
    case project::CommitOperationResultCategory::Success:
      return NotificationService::Tone::Info;
    case project::CommitOperationResultCategory::HookFailed:
    case project::CommitOperationResultCategory::DirtyWorktree:
    case project::CommitOperationResultCategory::RefreshFailedAfterSuccess:
      return NotificationService::Tone::Warning;
    case project::CommitOperationResultCategory::AuthFailed:
    case project::CommitOperationResultCategory::RepoLocked:
    case project::CommitOperationResultCategory::Conflict:
      return NotificationService::Tone::Error;
    default:
      return NotificationService::Tone::Error;
  }
}

}  // namespace

CommitWorkflowService::CommitWorkflowService(
    project::ProjectBackgroundExecutor& background_executor,
    GitRepositoryService& git_repository_service)
    : background_executor_(background_executor),
      git_repository_service_(git_repository_service) {}

void CommitWorkflowService::SetCallbacks(Callbacks callbacks) {
  std::lock_guard lock(mutex_);
  callbacks_ = std::move(callbacks);
}

project::CommitDraftContext CommitWorkflowService::CurrentDraftContext() const {
  const project::GitRepositoryState repository_state = git_repository_service_.CurrentState();
  return project::CommitDraftContext{
      .head_oid = repository_state.branch.head_oid,
      .branch_name = repository_state.branch.branch_name,
  };
}

void CommitWorkflowService::RestorePersistedDraft(
    CommitWorkflowState& state,
    const PersistedCommitDraftState& persisted) {
  const project::CommitDraftContext context = CurrentDraftContext();
  if (persisted.head_oid != context.head_oid || persisted.branch_name != context.branch_name) {
    return;
  }
  state.subject.SetText(persisted.subject);
  SetBodyText(state.body, persisted.body);
  state.draft_context = context;
  state.draft_restored = true;
}

PersistedCommitDraftState CommitWorkflowService::BuildPersistedDraft(
    const CommitWorkflowState& state) const {
  return PersistedCommitDraftState{
      .head_oid = state.draft_context.head_oid,
      .branch_name = state.draft_context.branch_name,
      .subject = state.subject.text(),
      .body = CommitWorkflowBodyText(state.body),
  };
}

void CommitWorkflowService::RefreshDerivedState(CommitWorkflowState& state,
                                                const bool run_blocking_conflict_scan) {
  const project::GitRepositoryState repository_state = git_repository_service_.CurrentState();
  // The staged summary depends only on the git index (which advances the git
  // generation when it changes), not on the subject/body being typed. Rebuild the
  // `git diff --cached --numstat` summary only when the generation moves so a
  // field-switch / warning-ack refresh does not re-run the subprocess redundantly.
  if (state.staged_summary_generation != repository_state.generation) {
    state.staged_summary = project::BuildCommitStagedSummary(repository_state);
    state.staged_summary_generation = repository_state.generation;
    if (state.staged_summary.file_count == 0) {
      state.staged_summary_line = "Nothing staged";
    } else {
      state.staged_summary_line =
          std::to_string(state.staged_summary.file_count) + " staged file(s), +" +
          std::to_string(state.staged_summary.added_lines) + "/-" +
          std::to_string(state.staged_summary.deleted_lines);
    }
  }
  // Pass the summary we just built so RunCommitPreChecks does not re-run the identical
  // `git diff --cached --numstat` subprocess on the shell thread. The unbounded
  // `git diff --cached` conflict-marker scan only runs when a commit is about to
  // dispatch (run_blocking_conflict_scan); interactive refreshes skip it for speed.
  state.checks = project::RunCommitPreChecks(
      repository_state, state.subject.text(), CommitWorkflowBodyText(state.body),
      state.acknowledged_warning_ids, &state.staged_summary, run_blocking_conflict_scan);
  if (callbacks_.request_commit_workflow_redraw != nullptr) {
    callbacks_.request_commit_workflow_redraw();
  }
}

void CommitWorkflowService::Open(CommitWorkflowState& state) {
  state.open = true;
  state.pending_confirmation = CommitWorkflowPendingConfirmation::None;
  state.operation_in_flight = false;
  state.acknowledged_warning_ids.clear();
  state.status_message.clear();
  state.last_hook_output.clear();
  state.focus_field = CommitWorkflowFocusField::Subject;
  state.body.SetPlaceholderText("Commit body (optional)");

  if (state.loaded_persisted_draft.has_value()) {
    RestorePersistedDraft(state, *state.loaded_persisted_draft);
    state.loaded_persisted_draft.reset();
  } else if (callbacks_.load_persisted_commit_draft != nullptr) {
    if (const auto persisted = callbacks_.load_persisted_commit_draft()) {
      RestorePersistedDraft(state, *persisted);
    }
  }

  state.draft_context = CurrentDraftContext();
  RefreshDerivedState(state);
  if (callbacks_.request_commit_workflow_redraw != nullptr) {
    callbacks_.request_commit_workflow_redraw();
  }
}

void CommitWorkflowService::Close(CommitWorkflowState& state) {
  if (state.open && callbacks_.persist_commit_draft != nullptr &&
      (!state.subject.text().empty() || !CommitWorkflowBodyText(state.body).empty())) {
    callbacks_.persist_commit_draft();
  }
  state.open = false;
  state.pending_confirmation = CommitWorkflowPendingConfirmation::None;
  state.operation_in_flight = false;
  if (callbacks_.request_commit_workflow_redraw != nullptr) {
    callbacks_.request_commit_workflow_redraw();
  }
}

void CommitWorkflowService::OnDraftEdited(CommitWorkflowState& state) {
  state.draft_restored = false;
  state.draft_context = CurrentDraftContext();
  RefreshDerivedState(state);
  if (callbacks_.persist_commit_draft != nullptr) {
    callbacks_.persist_commit_draft();
  }
}

void CommitWorkflowService::AcknowledgeWarning(CommitWorkflowState& state,
                                               const std::string_view warning_id) {
  if (!warning_id.empty()) {
    state.acknowledged_warning_ids.insert(std::string(warning_id));
  }
  RefreshDerivedState(state);
}

bool CommitWorkflowService::CanExecuteCommit(const CommitWorkflowState& state) const {
  return state.open && !state.operation_in_flight && state.staged_summary.file_count > 0 &&
         project::CommitPreChecksAllowExecution(state.checks, state.acknowledged_warning_ids);
}

bool CommitWorkflowService::RequestCommit(CommitWorkflowState& state,
                                          const project::CommitOperationKind operation) {
  if (!state.open || state.operation_in_flight) {
    return false;
  }
  // Pre-dispatch refresh runs the full conflict-marker scan; a positive result is a
  // blocking check that CanExecuteCommit rejects below.
  RefreshDerivedState(state, /*run_blocking_conflict_scan=*/true);
  if (!CanExecuteCommit(state)) {
    if (callbacks_.set_command_feedback != nullptr) {
      for (const project::CommitPreCheck& check : state.checks) {
        if (check.severity == project::CommitPreCheckSeverity::Blocking ||
            (check.severity == project::CommitPreCheckSeverity::Warning &&
             state.acknowledged_warning_ids.find(check.id) ==
                 state.acknowledged_warning_ids.end())) {
          callbacks_.set_command_feedback(check.message);
          break;
        }
      }
    }
    return false;
  }

  if (operation == project::CommitOperationKind::Amend ||
      operation == project::CommitOperationKind::NoVerify) {
    state.pending_confirmation = operation == project::CommitOperationKind::Amend
                                   ? CommitWorkflowPendingConfirmation::Amend
                                   : CommitWorkflowPendingConfirmation::NoVerify;
    if (callbacks_.open_commit_confirmation != nullptr) {
      callbacks_.open_commit_confirmation(operation);
    }
    return true;
  }

  DispatchCommit(state, operation);
  return true;
}

bool CommitWorkflowService::ConfirmPendingOperation(CommitWorkflowState& state) {
  if (state.pending_confirmation == CommitWorkflowPendingConfirmation::None) {
    return false;
  }
  const project::CommitOperationKind operation =
      state.pending_confirmation == CommitWorkflowPendingConfirmation::Amend
          ? project::CommitOperationKind::Amend
          : project::CommitOperationKind::NoVerify;
  state.pending_confirmation = CommitWorkflowPendingConfirmation::None;
  DispatchCommit(state, operation);
  return true;
}

void CommitWorkflowService::CancelPendingConfirmation(CommitWorkflowState& state) {
  state.pending_confirmation = CommitWorkflowPendingConfirmation::None;
}

void CommitWorkflowService::DispatchCommit(CommitWorkflowState& state,
                                           const project::CommitOperationKind operation) {
  const project::GitRepositoryState repository_state = git_repository_service_.CurrentState();
  if (repository_state.repository_root.empty() || !repository_state.repo_available) {
    return;
  }

  const std::string subject = state.subject.text();
  const std::string body = CommitWorkflowBodyText(state.body);
  const std::uint64_t repository_generation = repository_state.generation;
  state.operation_in_flight = true;
  state.status_message = "Committing...";

  std::uint64_t captured_generation = 0;
  {
    std::lock_guard lock(mutex_);
    captured_generation = ++operation_generation_;
  }
  background_executor_.Post([this, &state, operation, subject, body, repository_generation,
                             captured_generation,
                             repository_root = repository_state.repository_root]() {
    // Worker thread: only run the (possibly slow, hook-invoking) git commit and
    // produce a result. Mutating CommitWorkflowState here would race the main
    // thread, which reads state.subject/body/status_message while rendering the
    // commit overlay. Marshal the state mutation back to the render thread.
    project::CommitOperationResult result =
        project::ExecuteGitCommit(repository_root, subject, body, operation);
    completion_mailbox_.Post([this, &state, operation, repository_generation, captured_generation,
                              result = std::move(result)]() mutable {
      std::lock_guard completion_lock(mutex_);
      if (captured_generation != operation_generation_) {
        return;
      }
      PublishResult(state, std::move(result), operation, repository_generation);
    });
  });
}

void CommitWorkflowService::SetCompletionWakeEvent(std::uint32_t event_type) {
  completion_mailbox_.SetWakeEventType(event_type);
}

void CommitWorkflowService::DrainCompletions() { completion_mailbox_.Drain(); }

void CommitWorkflowService::PublishResult(CommitWorkflowState& state,
                                          project::CommitOperationResult result,
                                          const project::CommitOperationKind operation,
                                          const std::uint64_t repository_generation) {
  (void)operation;
  state.operation_in_flight = false;
  state.last_result_category = result.category;
  state.last_hook_output = result.hook_output;
  state.status_message = ResultFeedback(result);

  if (!result.hook_output.empty()) {
    if (callbacks_.append_output != nullptr) {
      callbacks_.append_output("git.commit", "Git Commit", result.hook_output);
    }
    if (callbacks_.show_output_panel != nullptr) {
      callbacks_.show_output_panel("git.commit");
    }
  }

  if (result.category == project::CommitOperationResultCategory::Success) {
    if (callbacks_.clear_persisted_commit_draft != nullptr) {
      callbacks_.clear_persisted_commit_draft();
    }
    state.subject.SetText({});
    SetBodyText(state.body, {});
    state.acknowledged_warning_ids.clear();
    state.draft_restored = false;
    git_repository_service_.MarkStale();
    if (callbacks_.request_git_refresh != nullptr) {
      callbacks_.request_git_refresh();
    }
    (void)repository_generation;
    Close(state);
  }

  RefreshDerivedState(state);
  if (callbacks_.set_command_feedback != nullptr) {
    callbacks_.set_command_feedback(state.status_message);
  }
  if (callbacks_.notify != nullptr) {
    callbacks_.notify(ResultTone(result.category), state.status_message);
  }
}

}  // namespace microide::workspace
