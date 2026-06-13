#include "workspace/WorkspaceShell.h"

#include "util/Parse.h"
#include "workspace/EditorTabService.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/PromptSurfaceService.h"
#include "workspace/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

PromptSurfaceService WorkspaceShell::MakePromptSurfaceService() {
  return PromptSurfaceService(
      context_.current_project_state,
      context_.prompts,
      PromptSurfaceService::Operations{
          .request_prompt_redraw = [this]() { RequestPromptRedraw(); },
      });
}

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  MakePromptSurfaceService().ShowDirtyPromptForTab(index);
}

void WorkspaceShell::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                            std::vector<std::size_t> dirty_tabs) {
  MakePromptSurfaceService().ShowDirtyPromptForTabs(std::move(target_tabs), std::move(dirty_tabs));
}

void WorkspaceShell::ShowDirtyPromptForProject(std::size_t index) {
  if (index >= context_.project_catalog.entries.size()) {
    return;
  }

  const std::vector<std::size_t> dirty_tabs = DirtyEditorTabIndicesForProject(index);
  if (dirty_tabs.empty()) {
    CloseProject(index);
    return;
  }

  MakePromptSurfaceService().ShowDirtyPromptForProject(index, dirty_tabs, dirty_tabs.size());
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  MakePromptSurfaceService().ShowDirtyPromptForQuit(context_.current_project_state.active_tab_index,
                                                    context_.project_catalog.active_index,
                                                    DirtyEditorTabIndices(),
                                                    dirty_count);
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  MakePromptSurfaceService().DismissDirtyPrompt(restore_focus);
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).Confirm();
}

std::array<std::string, 3> WorkspaceShell::DirtyPromptActionLabels() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return {
        context_.prompts.dirty.dirty_count > 1 ? "Save all" : "Save",
        context_.prompts.dirty.dirty_count > 1 ? "Discard all" : "Discard",
        "Cancel",
    };
  }

  return {"Save", "Discard", "Cancel"};
}

std::string WorkspaceShell::DirtyPromptTitle() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit) {
    return "Unsaved changes before quit";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return "Unsaved changes before closing tabs";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    return "Unsaved changes before closing project";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath) {
    return "Unsaved changes before rename";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return "Unsaved changes before delete";
  }
  return "Unsaved changes";
}

std::string WorkspaceShell::DirtyPromptMessage() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit) {
    const std::size_t dirty_count = context_.prompts.dirty.dirty_count;
    return dirty_count == 1 ? "Save the dirty tab before quitting microide?"
                            : "Save the " + std::to_string(dirty_count) +
                                  " dirty tabs before quitting microide?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    const std::filesystem::path project_root = ProjectCatalogRoot(context_.prompts.dirty.project_index);
    const std::string label = ProjectLabelForRoot(project_root);
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the dirty tab before closing " + label + "?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " dirty tabs before closing " + label + "?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the dirty tab before closing the selected tabs?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " dirty tabs before closing the selected tabs?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    const std::filesystem::path path =
        context_.prompts.dirty.path.empty() ? context_.prompts.surface.path : context_.prompts.dirty.path;
    const std::string label =
        path == context_.current_project_state.root ? ProjectLabel() : RelativePathLabel(context_.current_project_state.root, path);
    const std::string action =
        context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ? "renaming " : "deleting ";
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the affected dirty editor before " + action + label + "?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " affected dirty editors before " + action + label + "?";
  }

  const std::size_t index = context_.prompts.dirty.tab_index;
  const std::string label = index < context_.current_project_state.open_tabs.size() ? context_.current_project_state.open_tabs[index].title : "this tab";
  return "Save changes to " + label + " before closing it?";
}

void WorkspaceShell::OpenPromptSurface(PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string input) {
  MakePromptSurfaceService().OpenPromptSurface(action, kind, path, std::move(input));
}

void WorkspaceShell::OpenExternalUrlPrompt(std::string url) {
  MakePromptSurfaceService().OpenExternalUrlPrompt(std::move(url));
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  MakePromptSurfaceService().DismissPromptSurface(restore_focus);
}

std::string WorkspaceShell::PromptSurfaceTitle() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "New File";
    case PromptSurfaceState::Action::CreateDirectory:
      return "New Folder";
    case PromptSurfaceState::Action::RenamePath:
      return "Rename";
    case PromptSurfaceState::Action::DeletePath:
      return "Delete";
    case PromptSurfaceState::Action::DiscardGitChanges:
      return "Discard All Changes";
    case PromptSurfaceState::Action::DiscardGitEntry:
      return "Discard Git Changes";
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return "Discard Patch";
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return "Outgoing Base Ref";
    case PromptSurfaceState::Action::OpenExternalUrl:
      return "Open External Link";
    case PromptSurfaceState::Action::ConfirmCommitAmend:
      return "Amend Commit";
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return "Commit Without Hooks";
  }
  return "Prompt";
}

std::string WorkspaceShell::PromptSurfaceMessage() const {
  const std::string label =
      context_.prompts.surface.path == context_.current_project_state.root
          ? ProjectLabel()
          : RelativePathLabel(context_.current_project_state.root, context_.prompts.surface.path);
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::CreateDirectory:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::RenamePath:
      return "Enter a new path for " + label + ".";
    case PromptSurfaceState::Action::DeletePath:
      return "Move " + label + " to trash?";
    case PromptSurfaceState::Action::DiscardGitChanges:
      return "Discard all tracked, untracked, and conflicted changes in " + ProjectLabel() + "?";
    case PromptSurfaceState::Action::DiscardGitEntry: {
      const auto entry_index = util::ParseSize(context_.prompts.surface.input.text());
      if (!entry_index.has_value() ||
          *entry_index >= context_.current_project_state.sidebar.git.entries.size()) {
        return "Discard changes for the selected Git sidebar row?";
      }
      return BuildGitDiscardPreviewSummary(
          context_.current_project_state.sidebar.git.entries[*entry_index], ProjectLabel());
    }
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return context_.prompts.surface.detail.empty()
                 ? "Discard the selected changes from the working tree?"
                 : context_.prompts.surface.detail;
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return "Compare outgoing files against this ref.";
    case PromptSurfaceState::Action::OpenExternalUrl:
      return "Open " + context_.prompts.surface.detail + " in your browser?";
    case PromptSurfaceState::Action::ConfirmCommitAmend:
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return context_.prompts.surface.detail;
  }
  return {};
}

std::string WorkspaceShell::PromptSurfaceDetail() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
    case PromptSurfaceState::Action::OpenExternalUrl:
      return context_.prompts.surface.detail;
    default:
      return {};
  }
}

std::vector<std::string> WorkspaceShell::PromptSurfaceActionLabels() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return {"Create File", "Cancel"};
    case PromptSurfaceState::Action::CreateDirectory:
      return {"Create Folder", "Cancel"};
    case PromptSurfaceState::Action::RenamePath:
      return {"Rename", "Cancel"};
    case PromptSurfaceState::Action::DeletePath:
      return {"Delete", "Cancel"};
    case PromptSurfaceState::Action::DiscardGitChanges:
      return {"Discard All", "Cancel"};
    case PromptSurfaceState::Action::DiscardGitEntry:
      return {"Discard", "Cancel"};
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return {"Discard", "Cancel"};
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return {"Use Ref", "Cancel"};
    case PromptSurfaceState::Action::OpenExternalUrl:
      return {"Open Link", "Cancel"};
    case PromptSurfaceState::Action::ConfirmCommitAmend:
      return {"Amend", "Cancel"};
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return {"Commit", "Cancel"};
  }
  return {"OK", "Cancel"};
}

std::filesystem::path WorkspaceShell::TreeMutationBasePath(ActionSource source) const {
  if (context_.current_project_state.root.empty()) {
    return {};
  }
  if (source == ActionSource::ContextMenu && context_.menu_state.tree_context_menu.open &&
      context_.menu_state.tree_context_menu.target == TreeContextTargetKind::Background) {
    return context_.current_project_state.root;
  }

  std::filesystem::path path = ResolveTreeActionPath(source);
  if (path.empty()) {
    return context_.current_project_state.root;
  }

  std::error_code error;
  if (std::filesystem::is_directory(path, error) && !error) {
    return path.lexically_normal();
  }
  return path.parent_path().lexically_normal();
}

bool WorkspaceShell::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                               std::string* blocking_label) const {
  auto* shell = const_cast<WorkspaceShell*>(this);
  EditorTabService editor_tabs = shell->MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = shell->MakePromptSurfaceService();
  return shell->MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
      .HasDirtyEditorTabsForPath(path, blocking_label);
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakePathMutationCoordinator(editor_tabs, prompt_surfaces).CloseOpenTabsForPath(path);
}

void WorkspaceShell::ConfirmPromptSurface(DirtyPathResolution resolution) {
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::OpenExternalUrl) {
    const std::string url = context_.prompts.surface.detail;
    const bool opened = !url.empty() && OpenExternalUrl(url);
    (void)opened;
    // Always restore focus to the surface that owned it before the prompt; the
    // success branch previously passed `!opened` (== false), stranding keyboard
    // focus on the dismissed prompt and leaving input dead until the next click.
    MakePromptSurfaceService().DismissPromptSurface(true);
    return;
  }
  if (context_.prompts.surface_visible &&
      (context_.prompts.surface.action == PromptSurfaceState::Action::ConfirmCommitAmend ||
       context_.prompts.surface.action == PromptSurfaceState::Action::ConfirmCommitNoVerify)) {
    InitializeCommitWorkflowService();
    auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
    if (resolution != DirtyPathResolution::Discard) {
      commit_workflow_service_.ConfirmPendingOperation(workflow);
    } else {
      commit_workflow_service_.CancelPendingConfirmation(workflow);
    }
    MakePromptSurfaceService().DismissPromptSurface(false);
    context_.current_project_state.surface.focus = FocusTarget::Sidebar;
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::SetGitOutgoingBaseRef) {
    const std::string ref = context_.prompts.surface.input.text();
    if (ref.empty()) {
      return;
    }
    MakePromptSurfaceService().DismissPromptSurface(false);
    SetGitOutgoingBaseChoice(OutgoingBaseChoice{
        .kind = OutgoingBaseChoice::Kind::SpecificRef,
        .custom_ref = ref,
    });
    context_.current_project_state.surface.focus = FocusTarget::Sidebar;
    return;
  }
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakePathMutationCoordinator(editor_tabs, prompt_surfaces).ConfirmPromptSurface(resolution);
}

}  // namespace microide::workspace
