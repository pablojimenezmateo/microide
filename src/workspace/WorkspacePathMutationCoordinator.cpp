#include "workspace/WorkspacePathMutationCoordinator.h"

#include <filesystem>

#include "project/FileOperationService.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

PathMutationCoordinator::PathMutationCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void PathMutationCoordinator::ConfirmPromptSurface(WorkspaceShell::DirtyPathResolution resolution) {
  if (!shell_.prompts_.surface_visible) {
    return;
  }

  const WorkspaceShell::PromptSurfaceState state = shell_.prompts_.surface;
  if (state.selected_button == 1) {
    shell_.DismissPromptSurface(true);
    return;
  }

  if (state.kind == WorkspaceShell::PromptSurfaceState::Kind::TextInput) {
    if (state.input.empty()) {
      return;
    }

    std::filesystem::path typed_path(state.input);
    if (typed_path.is_absolute()) {
      return;
    }

    std::filesystem::path destination;
    if (state.action == WorkspaceShell::PromptSurfaceState::Action::RenamePath) {
      if (!ResolveDirtyTabsForPath(state.path, WorkspaceShell::DirtyPromptState::Kind::RenamePath,
                                   resolution)) {
        return;
      }
      destination = (state.path.parent_path() / typed_path).lexically_normal();
    } else {
      destination = (state.path / typed_path).lexically_normal();
    }

    if (!PathEqualsOrWithin(destination, shell_.project_root_)) {
      return;
    }

    project::FileOperationResult result;
    if (state.action == WorkspaceShell::PromptSurfaceState::Action::CreateFile) {
      result = project::FileOperationService::CreateFile(destination);
    } else if (state.action == WorkspaceShell::PromptSurfaceState::Action::CreateDirectory) {
      result = project::FileOperationService::CreateDirectory(destination);
    } else {
      result = project::FileOperationService::RenamePath(state.path, destination);
    }

    if (!result.ok) {
      return;
    }

    if (shell_.prompts_.dirty_visible) {
      shell_.DismissDirtyPrompt(false);
    }
    shell_.DismissPromptSurface(false);
    if (state.action == WorkspaceShell::PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      shell_.OpenFile(result.resulting_path);
      return;
    }
    if (state.action == WorkspaceShell::PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path,
                              resolution != WorkspaceShell::DirtyPathResolution::Discard);
    RetargetDiagnosticsForRename(state.path, result.resulting_path);
    shell_.ClearEditorBlame();
    RefreshProjectViewsAfterMutation(result.resulting_path);
    shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
    return;
  }

  if (state.action == WorkspaceShell::PromptSurfaceState::Action::DiscardGitChanges) {
    const bool discarded = shell_.DiscardAllGitSidebarEntries();
    shell_.DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
    }
    return;
  }

  if (!ResolveDirtyTabsForPath(state.path, WorkspaceShell::DirtyPromptState::Kind::DeletePath,
                               resolution)) {
    return;
  }

  const project::FileOperationResult result = project::FileOperationService::TrashPath(state.path);
  if (!result.ok) {
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  if (shell_.prompts_.dirty_visible) {
    shell_.DismissDirtyPrompt(false);
  }
  shell_.DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  ClearDiagnosticsForPath(state.path);
  shell_.ClearEditorBlame();
  RefreshProjectViewsAfterMutation(parent);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
}

}  // namespace microide::workspace
