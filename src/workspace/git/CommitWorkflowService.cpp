#include "workspace/git/CommitWorkflowService.h"

#include <utility>

#include "project/CommitWorkflowChecks.h"
#include "project/GitCommitExecutor.h"
#include "workspace/git/GitRepositoryService.h"

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
      repository_state, state.subject.text(), state.BodyText(),
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
      (!state.subject.text().empty() || !state.BodyText().empty())) {
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
  // A Blocking check refuses outright, every time. Nothing acknowledges these.
  for (const project::CommitPreCheck& check : state.checks) {
    if (check.severity == project::CommitPreCheckSeverity::Blocking) {
      if (callbacks_.set_command_feedback != nullptr) {
        callbacks_.set_command_feedback(check.message);
      }
      return false;
    }
  }
  // Nothing staged (or the surface is closed / already committing) is not a
  // pre-check; it is simply not a commit.
  if (!state.open || state.operation_in_flight || state.staged_summary.file_count == 0) {
    if (callbacks_.set_command_feedback != nullptr && state.staged_summary.file_count == 0) {
      callbacks_.set_command_feedback("Nothing staged");
    }
    return false;
  }
  // Whatever is left is unacknowledged WARNINGS. These are advisory — "untracked
  // files are not included", "branch is behind upstream" — and used to refuse the
  // commit outright with no way forward, because AcknowledgeWarning had no caller.
  // Ask once, then dispatch on confirmation.
  if (!CanExecuteCommit(state)) {
    std::string summary;
    for (const project::CommitPreCheck& check : state.checks) {
      if (check.severity != project::CommitPreCheckSeverity::Warning ||
          state.acknowledged_warning_ids.count(check.id) != 0) {
        continue;
      }
      if (!summary.empty()) {
        summary += "\n";
      }
      summary += check.message;
    }
    if (summary.empty()) {
      // No warning explains the refusal, so surfacing a confirmation would be a
      // dialog the user cannot act on. Refuse instead of prompting emptily.
      return false;
    }
    state.pending_operation = operation;
    state.pending_confirmation = CommitWorkflowPendingConfirmation::Warnings;
    if (callbacks_.open_commit_warning_confirmation != nullptr) {
      callbacks_.open_commit_warning_confirmation(std::move(summary));
    }
    return true;
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
  if (state.pending_confirmation == CommitWorkflowPendingConfirmation::Warnings) {
    // Record the acknowledgement for every warning currently outstanding, then run
    // the operation the user originally asked for. The ids are stable per check kind
    // (CheckId), so the re-run sees them acknowledged and proceeds; Open() and a
    // successful commit both clear the set, so the next commit asks again.
    const project::CommitOperationKind operation = state.pending_operation;
    state.pending_confirmation = CommitWorkflowPendingConfirmation::None;
    for (const project::CommitPreCheck& check : state.checks) {
      if (check.severity == project::CommitPreCheckSeverity::Warning) {
        state.acknowledged_warning_ids.insert(check.id);
      }
    }
    return RequestCommit(state, operation);
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
  const std::string body = state.BodyText();
  const std::uint64_t repository_generation = repository_state.generation;
  state.operation_in_flight = true;
  state.status_message = "Committing\xE2\x80\xA6";

  std::uint64_t captured_generation = 0;
  {
    std::lock_guard lock(mutex_);
    captured_generation = ++operation_generation_;
    // The operation in front of the user owns the feedback: drop any watch left
    // over from a previous commit whose refresh has not landed yet.
    post_commit_refresh_watch_ = 0;
  }
  // Let the state cancel this operation if it is destroyed before the completion
  // is drained — a project tab closing mid-commit. Without it the completion below
  // publishes into freed memory.
  state.in_flight_claim_ = CommitOperationClaim(this, captured_generation);
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
        // Superseded by a newer dispatch, or abandoned because the owning state was
        // destroyed. Either way `state` must not be touched — in the second case it
        // no longer exists.
        return;
      }
      // Reaching here proves the state is still alive and still the one that
      // dispatched: destroying, moving over, or copying it would have cancelled the
      // claim and bumped the generation. Release rather than cancel — the operation
      // finished, it was not abandoned.
      state.in_flight_claim_.Release();
      PublishResult(state, std::move(result), operation, repository_generation);
    });
  });
}

void CommitWorkflowService::AbandonOperation(const std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  // Only invalidate if this state's operation is still the current one. A state
  // whose commit already published, or was superseded by another project's, must
  // not cancel whatever is running now.
  if (generation != 0 && generation == operation_generation_) {
    ++operation_generation_;
  }
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
      // Arm the watch BEFORE asking, so a refresh that is dispatched immediately
      // is still >= the recorded floor. `+ 1` because the request may be
      // throttled or folded into a deferred follow-up, in which case the commit's
      // changes surface in a generation later than any this call mints.
      post_commit_refresh_watch_ = git_repository_service_.CurrentRefreshGeneration() + 1;
      callbacks_.request_git_refresh();
    }
    // `repository_generation` is the snapshot the commit was composed against; the
    // refresh correlation runs off the refresh generation instead, because that is
    // what a published snapshot carries.
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

void CommitWorkflowService::NoteGitRefreshOutcome(const std::uint64_t snapshot_generation,
                                                  const std::string_view refresh_error) {
  // Same locking shape as PublishResult: `callbacks_` is guarded by mutex_, and
  // both run on the main thread, so the callbacks fire under the lock.
  std::lock_guard lock(mutex_);
  if (post_commit_refresh_watch_ == 0 || snapshot_generation < post_commit_refresh_watch_) {
    return;
  }
  // This snapshot answers the commit's refresh either way; a later failure
  // belongs to whatever caused it, not to the commit.
  post_commit_refresh_watch_ = 0;
  if (refresh_error.empty()) {
    return;
  }

  project::CommitOperationResult result;
  result.category = project::CommitOperationResultCategory::RefreshFailedAfterSuccess;
  // `detail` is left empty on purpose: that is what routes through the category's
  // own branch in ResultFeedback rather than around it. The git text is appended
  // after, since the canonical sentence alone does not say why.
  std::string message = ResultFeedback(result);
  message += ": ";
  message += refresh_error;

  if (callbacks_.set_command_feedback != nullptr) {
    callbacks_.set_command_feedback(message);
  }
  if (callbacks_.notify != nullptr) {
    callbacks_.notify(ResultTone(result.category), std::move(message));
  }
}

}  // namespace microide::workspace
