#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  RequestPromptRedraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = surface_.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseTab;
  prompts_.dirty.tab_index = index;
  prompts_.dirty.target_tabs = {index};
  prompts_.dirty.dirty_tabs = {index};
  prompts_.dirty.dirty_count = 1;
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  surface_.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                            std::vector<std::size_t> dirty_tabs) {
  if (target_tabs.empty() || dirty_tabs.empty()) {
    return;
  }

  RequestPromptRedraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = surface_.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseTabs;
  prompts_.dirty.tab_index = target_tabs.front();
  prompts_.dirty.target_tabs = std::move(target_tabs);
  prompts_.dirty.dirty_tabs = std::move(dirty_tabs);
  prompts_.dirty.dirty_count = prompts_.dirty.dirty_tabs.size();
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  surface_.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::ShowDirtyPromptForProject(std::size_t index) {
  if (index >= project_catalog_.entries.size()) {
    return;
  }

  const std::vector<std::size_t> dirty_tabs = DirtyEditorTabIndicesForProject(index);
  if (dirty_tabs.empty()) {
    CloseProject(index);
    return;
  }

  RequestPromptRedraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = surface_.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseProject;
  prompts_.dirty.project_index = index;
  prompts_.dirty.dirty_tabs = dirty_tabs;
  prompts_.dirty.dirty_count = dirty_tabs.size();
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  surface_.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < project_catalog_.entries.size(); ++i) {
    if (HasActiveProjectCatalogEntry() && i == project_catalog_.active_index) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  RequestPromptRedraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = surface_.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::Quit;
  prompts_.dirty.tab_index = active_tab_index_;
  prompts_.dirty.project_index = project_catalog_.active_index;
  prompts_.dirty.dirty_tabs = DirtyEditorTabIndices();
  prompts_.dirty.dirty_count = dirty_count;
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  surface_.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  RequestPromptRedraw();
  prompts_.dirty_visible = false;
  prompts_.dirty = DirtyPromptState{};
  if (restore_focus) {
    surface_.focus = prompts_.dirty_previous_focus;
  }
  RequestPromptRedraw();
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  MakeDirtyPromptCoordinator().Confirm();
}

std::array<std::string, 3> WorkspaceShell::DirtyPromptActionLabels() const {
  if (prompts_.dirty.kind == DirtyPromptState::Kind::Quit ||
      prompts_.dirty.kind == DirtyPromptState::Kind::CloseTabs ||
      prompts_.dirty.kind == DirtyPromptState::Kind::CloseProject ||
      prompts_.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      prompts_.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return {
        prompts_.dirty.dirty_count > 1 ? "Save all" : "Save",
        prompts_.dirty.dirty_count > 1 ? "Discard all" : "Discard",
        "Cancel",
    };
  }

  return {"Save", "Discard", "Cancel"};
}

std::string WorkspaceShell::DirtyPromptTitle() const {
  if (prompts_.dirty.kind == DirtyPromptState::Kind::Quit) {
    return "Unsaved changes before quit";
  }
  if (prompts_.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return "Unsaved changes before closing tabs";
  }
  if (prompts_.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    return "Unsaved changes before closing project";
  }
  if (prompts_.dirty.kind == DirtyPromptState::Kind::RenamePath) {
    return "Unsaved changes before rename";
  }
  if (prompts_.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return "Unsaved changes before delete";
  }
  return "Unsaved changes";
}

std::string WorkspaceShell::DirtyPromptMessage() const {
  if (prompts_.dirty.kind == DirtyPromptState::Kind::Quit) {
    const std::size_t dirty_count = prompts_.dirty.dirty_count;
    return dirty_count == 1 ? "Save the dirty tab before quitting microide?"
                            : "Save the " + std::to_string(dirty_count) +
                                  " dirty tabs before quitting microide?";
  }

  if (prompts_.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    const std::filesystem::path project_root = ProjectCatalogRoot(prompts_.dirty.project_index);
    const std::string label = ProjectLabelForRoot(project_root);
    return prompts_.dirty.dirty_count == 1
               ? "Save the dirty tab before closing " + label + "?"
               : "Save the " + std::to_string(prompts_.dirty.dirty_count) +
                     " dirty tabs before closing " + label + "?";
  }

  if (prompts_.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return prompts_.dirty.dirty_count == 1
               ? "Save the dirty tab before closing the selected tabs?"
               : "Save the " + std::to_string(prompts_.dirty.dirty_count) +
                     " dirty tabs before closing the selected tabs?";
  }

  if (prompts_.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      prompts_.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    const std::filesystem::path path =
        prompts_.dirty.path.empty() ? prompts_.surface.path : prompts_.dirty.path;
    const std::string label =
        path == project_root_ ? ProjectLabel() : RelativePathLabel(project_root_, path);
    const std::string action =
        prompts_.dirty.kind == DirtyPromptState::Kind::RenamePath ? "renaming " : "deleting ";
    return prompts_.dirty.dirty_count == 1
               ? "Save the affected dirty editor before " + action + label + "?"
               : "Save the " + std::to_string(prompts_.dirty.dirty_count) +
                     " affected dirty editors before " + action + label + "?";
  }

  const std::size_t index = prompts_.dirty.tab_index;
  const std::string label = index < open_tabs_.size() ? open_tabs_[index].title : "this tab";
  return "Save changes to " + label + " before closing it?";
}

void WorkspaceShell::OpenPromptSurface(PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string input) {
  RequestPromptRedraw();
  prompts_.surface_visible = true;
  prompts_.surface_previous_focus = surface_.focus;
  prompts_.surface.kind = kind;
  prompts_.surface.action = action;
  prompts_.surface.path = path.lexically_normal();
  prompts_.surface.input = std::move(input);
  prompts_.surface.selected_button = 0;
  surface_.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  RequestPromptRedraw();
  prompts_.surface_visible = false;
  prompts_.surface = PromptSurfaceState{};
  if (restore_focus) {
    surface_.focus = prompts_.surface_previous_focus;
  }
  RequestPromptRedraw();
}

std::string WorkspaceShell::PromptSurfaceTitle() const {
  switch (prompts_.surface.action) {
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
  }
  return "Prompt";
}

std::string WorkspaceShell::PromptSurfaceMessage() const {
  const std::string label =
      prompts_.surface.path == project_root_
          ? ProjectLabel()
          : RelativePathLabel(project_root_, prompts_.surface.path);
  switch (prompts_.surface.action) {
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
  }
  return {};
}

std::array<std::string, 2> WorkspaceShell::PromptSurfaceActionLabels() const {
  switch (prompts_.surface.action) {
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
  }
  return {"OK", "Cancel"};
}

std::filesystem::path WorkspaceShell::TreeMutationBasePath(ActionSource source) const {
  if (project_root_.empty()) {
    return {};
  }
  if (source == ActionSource::ContextMenu && menu_state_.tree_context_menu.open &&
      menu_state_.tree_context_menu.target == TreeContextTargetKind::Background) {
    return project_root_;
  }

  std::filesystem::path path = ResolveTreeActionPath(source);
  if (path.empty()) {
    return project_root_;
  }

  std::error_code error;
  if (std::filesystem::is_directory(path, error) && !error) {
    return path.lexically_normal();
  }
  return path.parent_path().lexically_normal();
}

bool WorkspaceShell::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                               std::string* blocking_label) const {
  return const_cast<WorkspaceShell*>(this)->MakePathMutationCoordinator().HasDirtyEditorTabsForPath(
      path, blocking_label);
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  MakePathMutationCoordinator().CloseOpenTabsForPath(path);
}

void WorkspaceShell::ConfirmPromptSurface(DirtyPathResolution resolution) {
  MakePathMutationCoordinator().ConfirmPromptSurface(resolution);
}

}  // namespace microide::workspace
