#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "platform/ProcessLauncher.h"
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
    std::function<const platform::ProcessLauncher*()> project_launcher;
    std::function<void()> request_git_refresh;
    std::function<void(std::string_view channel_id, std::string_view label, std::string line)>
        append_output;
    std::function<void(std::string_view)> show_output_panel;
    std::function<void(std::string_view)> set_command_feedback;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> persist_commit_draft;
    std::function<void(CommitWorkflowState& state)> clear_persisted_commit_draft;
    std::function<std::optional<PersistedCommitDraftState>()> load_persisted_commit_draft;
    std::function<void(const std::filesystem::path& path, std::string_view left_ref,
                       std::string_view left_label)>
        open_staged_diff;
    std::function<void(project::CommitOperationKind)> open_commit_confirmation;
    std::function<void(std::string)> open_commit_warning_confirmation;
    std::function<void()> request_commit_workflow_redraw;
    std::function<bool(const std::filesystem::path& project_root)> is_in_front;
    std::function<std::string(const std::filesystem::path& project_root)> project_label;
    std::function<void(const std::filesystem::path& project_root)> defer_to_project;
    std::function<std::filesystem::path()> front_project_root;
    std::function<CommitWorkflowState*(const std::filesystem::path& project_root)> state_for_project;
  };

  explicit CommitWorkflowService(project::ProjectBackgroundExecutor& background_executor,
                                 GitRepositoryService& git_repository_service);

  void SetCallbacks(Callbacks callbacks);

  void SetCompletionWakeEvent(std::uint32_t event_type);
  void DrainCompletions();

  int PendingCompletionCount() const { return completion_mailbox_.PendingCount(); }

  void Open(CommitWorkflowState& state);
  void Close(CommitWorkflowState& state);
  void RefreshDerivedState(CommitWorkflowState& state, bool run_blocking_conflict_scan = false);

  const platform::ProcessLauncher& ProjectLauncher() const;
  void OnDraftEdited(CommitWorkflowState& state);
  bool RequestCommit(CommitWorkflowState& state, project::CommitOperationKind operation);
  bool ConfirmPendingOperation(CommitWorkflowState& state);
  void CancelPendingConfirmation(CommitWorkflowState& state);
  bool CanExecuteCommit(const CommitWorkflowState& state) const;
  void RestorePersistedDraft(CommitWorkflowState& state,
                             const PersistedCommitDraftState& persisted);

  void AbandonOperation(std::uint64_t generation);

  void NoteGitRefreshOutcome(CommitWorkflowState& state, std::uint64_t snapshot_generation,
                             std::string_view refresh_error);

 private:
  bool RequestCommitImpl(CommitWorkflowState& state, project::CommitOperationKind operation,
                         bool may_wait_for_refresh);
  void ReportRefusal(std::string message) const;
  void DispatchCommit(CommitWorkflowState& state, project::CommitOperationKind operation);
  void PublishResult(CommitWorkflowState& state, const std::filesystem::path& project_root,
                     project::CommitOperationResult result, project::CommitOperationKind operation,
                     std::uint64_t repository_generation);
  project::CommitDraftContext CurrentDraftContext() const;

  project::ProjectBackgroundExecutor& background_executor_;
  GitRepositoryService& git_repository_service_;
  Callbacks callbacks_;
  mutable std::mutex mutex_;
  std::uint64_t operation_generation_ = 0;
  std::uint64_t post_commit_refresh_watch_ = 0;
  std::uint64_t pre_commit_refresh_watch_ = 0;
  project::CommitOperationKind commit_waiting_for_refresh_ = project::CommitOperationKind::Create;
  util::MainThreadMailbox completion_mailbox_;
};

}
