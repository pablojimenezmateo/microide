#include "workspace/WorkspaceShell.h"

#include "workspace/CommitWorkflowService.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

namespace {

// Tall enough to hold the framed subject field plus a multi-line body edit area (and the
// pre-check/status lines beneath). The body shows several lines and scrolls internally.
constexpr float kCommitWorkflowPanelHeight = 268.0f;

}  // namespace

void WorkspaceShell::InitializeCommitWorkflowService() {
  commit_workflow_service_.SetCallbacks(CommitWorkflowService::Callbacks{
      .request_git_refresh = [this]() { RequestAutomaticGitSidebarRefresh(); },
      .append_output =
          [this](const std::string_view channel_id, const std::string_view label,
                 const std::string line) {
            output_channels_.AppendLine(channel_id, label, line);
          },
      .show_output_panel =
          [this](const std::string_view channel_id) {
            EnsureOutputChannelTabOpen(channel_id);
            context_.current_project_state.panel.content = PanelContentKind::Output;
            context_.current_project_state.panel.output.channel_id = std::string(channel_id);
            context_.current_project_state.surface.focus = FocusTarget::Panel;
          },
      .set_command_feedback =
          [this](const std::string_view feedback) {
            context_.current_project_state.panel.command.feedback_text = std::string(feedback);
          },
      .persist_commit_draft = [this]() { MakePersistenceCoordinator().SaveConfigState(); },
      .clear_persisted_commit_draft =
          [this]() {
            auto& draft = context_.current_project_state.sidebar.git.commit_workflow;
            draft.subject.SetText({});
            draft.body.LoadContent({});
            draft.loaded_persisted_draft.reset();
            MakePersistenceCoordinator().SaveConfigState();
          },
      .load_persisted_commit_draft =
          [this]() -> std::optional<PersistedCommitDraftState> {
            return context_.current_project_state.sidebar.git.commit_workflow.loaded_persisted_draft;
          },
      .open_staged_diff =
          [this](const std::filesystem::path& path, const std::string_view left_ref,
                 const std::string_view left_label) {
            OpenWorkingTreeComparison(path, std::string(left_ref), std::string(left_label));
          },
      .open_commit_confirmation =
          [this](const project::CommitOperationKind operation) {
            const bool amend = operation == project::CommitOperationKind::Amend;
            OpenPromptSurface(
                amend ? PromptSurfaceState::Action::ConfirmCommitAmend
                      : PromptSurfaceState::Action::ConfirmCommitNoVerify,
                PromptSurfaceState::Kind::Confirm, {}, {});
            context_.prompts.surface.detail =
                amend ? "Rewrite the previous commit with the staged changes and new message?"
                      : "Bypass Git hooks for this commit? Only continue if you trust the staged changes.";
          },
      .request_commit_workflow_redraw = [this]() { RequestSidebarRedraw(); },
  });
}

bool WorkspaceShell::OpenCommitWorkflow() {
  if (!context_.current_project_state.sidebar.visible ||
      ActiveSidebarMode() != SidebarMode::Git) {
    ShowGitSidebar();
  }
  InitializeCommitWorkflowService();
  commit_workflow_service_.Open(context_.current_project_state.sidebar.git.commit_workflow);
  context_.current_project_state.surface.focus = FocusTarget::Sidebar;
  RequestSidebarRedraw();
  return true;
}

void WorkspaceShell::CloseCommitWorkflow() {
  if (!context_.current_project_state.sidebar.git.commit_workflow.open) {
    return;
  }
  commit_workflow_service_.Close(context_.current_project_state.sidebar.git.commit_workflow);
  RequestSidebarRedraw();
}

float WorkspaceShell::GitSidebarCommitWorkflowHeight() const {
  if (!context_.current_project_state.sidebar.git.commit_workflow.open) {
    return 0.0f;
  }
  return kCommitWorkflowPanelHeight;
}

bool WorkspaceShell::CommitWorkflowOpen() const {
  return context_.current_project_state.sidebar.git.commit_workflow.open;
}

}  // namespace microide::workspace
