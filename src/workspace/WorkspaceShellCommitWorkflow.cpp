#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/CommitWorkflowLayout.h"
#include "workspace/CommitWorkflowService.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

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
            context_.current_project_state.panel.feedback.text = std::string(feedback);
          },
      .notify =
          [this](NotificationService::Tone tone, std::string message) {
            Notify(tone, std::move(message));
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
  const auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
  if (!workflow.open) {
    return 0.0f;
  }
  // Reserve exactly what the sidebar render path draws (subject/body fields, pre-check and
  // status lines, confirm button). Field heights track the live line height, and the check
  // count varies, so this is computed rather than a fixed constant. The field_x/field_w args
  // don't affect total_height, so passing 0/0 is fine here.
  return ComputeCommitWorkflowLayout(0.0f, 0.0f, 0.0f, text_renderer_.LineHeight(),
                                     kCommitWorkflowBodyRows, workflow.checks.size(),
                                     !workflow.status_message.empty())
      .total_height;
}

void WorkspaceShell::PrepareCommitBodyViewportForFrame(const SDL_FRect& sidebar_rect) {
  // Frame-prep half of the commit-draft body field (TD-2026-07-17-083 residual):
  // size the body viewport to the field and keep the caret row visible BEFORE
  // paint, so RenderCommitBodyField is a pure draw. The field width is
  // sidebar-width-derived (see ComputeCommitWorkflowLayout: body_field.w ==
  // field_w == sidebar width minus the shared inset), so no panel origin is
  // needed here.
  auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
  if (!workflow.open) {
    return;
  }
  const float field_w = std::max(0.0f, sidebar_rect.w - kCommitWorkflowFieldInset * 2.0f);
  const float avail_w = std::max(1.0f, field_w - 12.0f);
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const std::size_t rows = static_cast<std::size_t>(std::max(1, kCommitWorkflowBodyRows));
  workflow.body.SetViewportSize(rows,
                                static_cast<std::size_t>(std::max(1.0f, avail_w / char_width)));
  std::size_t scroll = workflow.body.scroll_line();
  const std::size_t caret_line = workflow.body.cursor_line();
  if (caret_line < scroll) {
    scroll = caret_line;
  } else if (caret_line >= scroll + rows) {
    scroll = caret_line - rows + 1;
  }
  workflow.body.SetScrollLine(scroll);
}

bool WorkspaceShell::CommitWorkflowOpen() const {
  return context_.current_project_state.sidebar.git.commit_workflow.open;
}

bool WorkspaceShell::RequestCommitWorkflowCommit() {
  InitializeCommitWorkflowService();
  return commit_workflow_service_.RequestCommit(
      context_.current_project_state.sidebar.git.commit_workflow,
      project::CommitOperationKind::Create);
}

}  // namespace microide::workspace
