#include "workspace/WorkspaceShell.h"

#include "util/SingleLineText.h"
#include "workspace/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return;
  }

  RequestPromptRedraw();
  context_.prompts.dirty_visible = true;
  context_.prompts.dirty_previous_focus = context_.current_project_state.surface.focus;
  context_.prompts.dirty.kind = DirtyPromptState::Kind::CloseTab;
  context_.prompts.dirty.tab_index = index;
  context_.prompts.dirty.target_tabs = {index};
  context_.prompts.dirty.dirty_tabs = {index};
  context_.prompts.dirty.dirty_count = 1;
  context_.prompts.dirty.path.clear();
  context_.prompts.dirty.selected_action = 0;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                            std::vector<std::size_t> dirty_tabs) {
  if (target_tabs.empty() || dirty_tabs.empty()) {
    return;
  }

  RequestPromptRedraw();
  context_.prompts.dirty_visible = true;
  context_.prompts.dirty_previous_focus = context_.current_project_state.surface.focus;
  context_.prompts.dirty.kind = DirtyPromptState::Kind::CloseTabs;
  context_.prompts.dirty.tab_index = target_tabs.front();
  context_.prompts.dirty.target_tabs = std::move(target_tabs);
  context_.prompts.dirty.dirty_tabs = std::move(dirty_tabs);
  context_.prompts.dirty.dirty_count = context_.prompts.dirty.dirty_tabs.size();
  context_.prompts.dirty.path.clear();
  context_.prompts.dirty.selected_action = 0;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
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

  RequestPromptRedraw();
  context_.prompts.dirty_visible = true;
  context_.prompts.dirty_previous_focus = context_.current_project_state.surface.focus;
  context_.prompts.dirty.kind = DirtyPromptState::Kind::CloseProject;
  context_.prompts.dirty.project_index = index;
  context_.prompts.dirty.dirty_tabs = dirty_tabs;
  context_.prompts.dirty.dirty_count = dirty_tabs.size();
  context_.prompts.dirty.path.clear();
  context_.prompts.dirty.selected_action = 0;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  RequestPromptRedraw();
  context_.prompts.dirty_visible = true;
  context_.prompts.dirty_previous_focus = context_.current_project_state.surface.focus;
  context_.prompts.dirty.kind = DirtyPromptState::Kind::Quit;
  context_.prompts.dirty.tab_index = context_.current_project_state.active_tab_index;
  context_.prompts.dirty.project_index = context_.project_catalog.active_index;
  context_.prompts.dirty.dirty_tabs = DirtyEditorTabIndices();
  context_.prompts.dirty.dirty_count = dirty_count;
  context_.prompts.dirty.path.clear();
  context_.prompts.dirty.selected_action = 0;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  RequestPromptRedraw();
  context_.prompts.dirty_visible = false;
  context_.prompts.dirty = DirtyPromptState{};
  if (restore_focus) {
    context_.current_project_state.surface.focus = context_.prompts.dirty_previous_focus;
  }
  RequestPromptRedraw();
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  MakeDirtyPromptCoordinator().Confirm();
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
  RequestPromptRedraw();
  context_.prompts.surface_visible = true;
  context_.prompts.surface_previous_focus = context_.current_project_state.surface.focus;
  context_.prompts.surface.kind = kind;
  context_.prompts.surface.action = action;
  context_.prompts.surface.path = path.lexically_normal();
  util::SetSingleLineText(&context_.prompts.surface.input, std::move(input));
  context_.prompts.surface.selected_button = 0;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  RequestPromptRedraw();
  context_.prompts.surface_visible = false;
  context_.prompts.surface = PromptSurfaceState{};
  if (restore_focus) {
    context_.current_project_state.surface.focus = context_.prompts.surface_previous_focus;
  }
  RequestPromptRedraw();
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
  }
  return {};
}

std::array<std::string, 2> WorkspaceShell::PromptSurfaceActionLabels() const {
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
