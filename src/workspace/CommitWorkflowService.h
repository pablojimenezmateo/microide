#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "project/CommitWorkflowTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/CommitWorkflowPersistence.h"
#include "workspace/CommitWorkflowState.h"
#include "workspace/NotificationService.h"

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
    std::function<void()> request_commit_workflow_redraw;
  };

  explicit CommitWorkflowService(project::ProjectBackgroundExecutor& background_executor,
                                 GitRepositoryService& git_repository_service);

  void SetCallbacks(Callbacks callbacks);

  void Open(CommitWorkflowState& state);
  void Close(CommitWorkflowState& state);
  void RefreshDerivedState(CommitWorkflowState& state);
  void OnDraftEdited(CommitWorkflowState& state);
  void AcknowledgeWarning(CommitWorkflowState& state, std::string_view warning_id);
  bool RequestCommit(CommitWorkflowState& state, project::CommitOperationKind operation);
  bool ConfirmPendingOperation(CommitWorkflowState& state);
  void CancelPendingConfirmation(CommitWorkflowState& state);
  bool CanExecuteCommit(const CommitWorkflowState& state) const;
  PersistedCommitDraftState BuildPersistedDraft(const CommitWorkflowState& state) const;
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
};

}  // namespace microide::workspace
