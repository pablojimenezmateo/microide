#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "editor/SyntaxHighlighter.h"
#include "project/FileOperationService.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::CloseTab;
  dirty_prompt_state_.tab_index = index;
  dirty_prompt_state_.dirty_tabs = {index};
  dirty_prompt_state_.dirty_count = 1;
  dirty_prompt_state_.path.clear();
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::ShowDirtyPromptForProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }

  const std::vector<std::size_t> dirty_tabs = DirtyEditorTabIndicesForProject(index);
  if (dirty_tabs.empty()) {
    CloseProject(index);
    return;
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::CloseProject;
  dirty_prompt_state_.project_index = index;
  dirty_prompt_state_.dirty_tabs = dirty_tabs;
  dirty_prompt_state_.dirty_count = dirty_tabs.size();
  dirty_prompt_state_.path.clear();
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (!project_root_.empty() && i == active_project_index_) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::Quit;
  dirty_prompt_state_.tab_index = active_tab_index_;
  dirty_prompt_state_.project_index = active_project_index_;
  dirty_prompt_state_.dirty_tabs = DirtyEditorTabIndices();
  dirty_prompt_state_.dirty_count = dirty_count;
  dirty_prompt_state_.path.clear();
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  dirty_prompt_visible_ = false;
  dirty_prompt_state_ = DirtyPromptState{};
  if (restore_focus) {
    focus_ = dirty_prompt_previous_focus_;
  }
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  if (!dirty_prompt_visible_) {
    return;
  }

  const DirtyPromptState prompt = dirty_prompt_state_;
  if (prompt.selected_action == 2) {
    if (prompt.kind == DirtyPromptState::Kind::RenamePath ||
        prompt.kind == DirtyPromptState::Kind::DeletePath) {
      DismissDirtyPrompt(true);
      return;
    }
    DismissDirtyPrompt(true);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::RenamePath) {
    ConfirmPromptSurface(prompt.selected_action == 0 ? DirtyPathResolution::Save
                                                     : DirtyPathResolution::Discard);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::DeletePath) {
    ConfirmPromptSurface(prompt.selected_action == 0 ? DirtyPathResolution::Save
                                                     : DirtyPathResolution::Discard);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::CloseTab) {
    if (prompt.selected_action == 0 && !SaveTab(prompt.tab_index)) {
      return;
    }
    DismissDirtyPrompt(false);
    CloseTab(prompt.tab_index);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::CloseProject) {
    if (prompt.project_index >= projects_.size()) {
      DismissDirtyPrompt(true);
      return;
    }
    if (prompt.selected_action == 0 &&
        (prompt.project_index != active_project_index_ || project_root_.empty())) {
      if (!SwitchProject(prompt.project_index, false)) {
        DismissDirtyPrompt(true);
        return;
      }
    }
    if (prompt.selected_action == 0) {
      for (std::size_t index : prompt.dirty_tabs) {
        if (!SaveTab(index)) {
          return;
        }
      }
    }
    DismissDirtyPrompt(false);
    CloseProject(active_project_index_);
    return;
  }

  if (prompt.selected_action == 0) {
    const std::size_t project_count = projects_.size();
    const std::size_t original_active_index = active_project_index_;
    const bool had_active_project = !project_root_.empty();
    for (std::size_t i = 0; i < project_count; ++i) {
      if (i >= projects_.size()) {
        break;
      }
      if (!SwitchProject(i, false)) {
        continue;
      }
      for (std::size_t index : DirtyEditorTabIndices()) {
        if (!SaveTab(index)) {
          return;
        }
      }
    }
    if (had_active_project && original_active_index < projects_.size()) {
      SwitchProject(original_active_index, false);
    }
  }

  DismissDirtyPrompt(false);
  quit_requested_ = true;
}

std::array<std::string, 3> WorkspaceShell::DirtyPromptActionLabels() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit ||
      dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject ||
      dirty_prompt_state_.kind == DirtyPromptState::Kind::RenamePath ||
      dirty_prompt_state_.kind == DirtyPromptState::Kind::DeletePath) {
    return {
        dirty_prompt_state_.dirty_count > 1 ? "Save all" : "Save",
        dirty_prompt_state_.dirty_count > 1 ? "Discard all" : "Discard",
        "Cancel",
    };
  }

  return {"Save", "Discard", "Cancel"};
}

std::string WorkspaceShell::DirtyPromptTitle() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit) {
    return "Unsaved changes before quit";
  }
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject) {
    return "Unsaved changes before closing project";
  }
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::RenamePath) {
    return "Unsaved changes before rename";
  }
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::DeletePath) {
    return "Unsaved changes before delete";
  }
  return "Unsaved changes";
}

std::string WorkspaceShell::DirtyPromptMessage() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit) {
    const std::size_t dirty_count = dirty_prompt_state_.dirty_count;
    return dirty_count == 1 ? "Save the dirty tab before quitting microide?"
                            : "Save the " + std::to_string(dirty_count) +
                                  " dirty tabs before quitting microide?";
  }

  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject) {
    const std::filesystem::path project_root =
        dirty_prompt_state_.project_index < projects_.size() &&
                projects_[dirty_prompt_state_.project_index] != nullptr
            ? projects_[dirty_prompt_state_.project_index]->root
            : project_root_;
    const std::string label = ProjectLabelForRoot(project_root);
    return dirty_prompt_state_.dirty_count == 1
               ? "Save the dirty tab before closing " + label + "?"
               : "Save the " + std::to_string(dirty_prompt_state_.dirty_count) +
                     " dirty tabs before closing " + label + "?";
  }

  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::RenamePath ||
      dirty_prompt_state_.kind == DirtyPromptState::Kind::DeletePath) {
    const std::filesystem::path path =
        dirty_prompt_state_.path.empty() ? prompt_surface_state_.path : dirty_prompt_state_.path;
    const std::string label =
        path == project_root_ ? ProjectLabel() : RelativePathLabel(project_root_, path);
    const std::string action =
        dirty_prompt_state_.kind == DirtyPromptState::Kind::RenamePath ? "renaming " : "deleting ";
    return dirty_prompt_state_.dirty_count == 1
               ? "Save the affected dirty editor before " + action + label + "?"
               : "Save the " + std::to_string(dirty_prompt_state_.dirty_count) +
                     " affected dirty editors before " + action + label + "?";
  }

  const std::size_t index = dirty_prompt_state_.tab_index;
  const std::string label = index < open_tabs_.size() ? open_tabs_[index].title : "this tab";
  return "Save changes to " + label + " before closing it?";
}

void WorkspaceShell::OpenPromptSurface(PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string input) {
  prompt_surface_visible_ = true;
  prompt_surface_previous_focus_ = focus_;
  prompt_surface_state_.kind = kind;
  prompt_surface_state_.action = action;
  prompt_surface_state_.path = path.lexically_normal();
  prompt_surface_state_.input = std::move(input);
  prompt_surface_state_.selected_button = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  prompt_surface_visible_ = false;
  prompt_surface_state_ = PromptSurfaceState{};
  if (restore_focus) {
    focus_ = prompt_surface_previous_focus_;
  }
}

std::string WorkspaceShell::PromptSurfaceTitle() const {
  switch (prompt_surface_state_.action) {
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
      prompt_surface_state_.path == project_root_
          ? ProjectLabel()
          : RelativePathLabel(project_root_, prompt_surface_state_.path);
  switch (prompt_surface_state_.action) {
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
  switch (prompt_surface_state_.action) {
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
  if (source == ActionSource::ContextMenu && tree_context_menu_.open &&
      tree_context_menu_.target == TreeContextTargetKind::Background) {
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

bool WorkspaceShell::EditorTabReferencesPath(std::size_t tab_index,
                                             const std::filesystem::path& path) const {
  if (tab_index >= open_tabs_.size()) {
    return false;
  }
  const TabEntry& tab = open_tabs_[tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  for (const auto& view : tab.editor_state->views) {
    const bool active_live_view =
        tab_index == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
        !view.needs_restore;
    const std::filesystem::path current_path =
        active_live_view ? text_viewport_.path().lexically_normal() : EditorViewPath(view);
    if (!current_path.empty() && PathEqualsOrWithin(current_path, path.lexically_normal())) {
      return true;
    }
  }
  return false;
}

bool WorkspaceShell::EditorTabHasDirtyPath(std::size_t tab_index,
                                           const std::filesystem::path& path) const {
  if (tab_index >= open_tabs_.size()) {
    return false;
  }
  const TabEntry& tab = open_tabs_[tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  for (const auto& view : tab.editor_state->views) {
    const bool active_live_view =
        tab_index == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
        !view.needs_restore;
    const editor::TextViewport& viewport = active_live_view ? text_viewport_ : view.viewport;
    if (view.needs_restore || viewport.path().empty() || !viewport.dirty()) {
      continue;
    }
    if (PathEqualsOrWithin(viewport.path().lexically_normal(), path.lexically_normal())) {
      return true;
    }
  }
  return false;
}

std::vector<WorkspaceShell::DirtyPathTarget> WorkspaceShell::DirtyPathTargetsForPath(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<DirtyPathTarget> targets;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        const bool active_live_view =
            i == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
            !view.needs_restore;
        const editor::TextViewport& viewport = active_live_view ? text_viewport_ : view.viewport;
        if (view.needs_restore || viewport.path().empty() || !viewport.dirty()) {
          continue;
        }
        if (PathEqualsOrWithin(viewport.path().lexically_normal(), normalized_path)) {
          targets.push_back(DirtyPathTarget{
              .kind = DirtyPathTarget::Kind::EditorView,
              .tab_index = i,
              .leaf_id = view.leaf_id,
          });
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_editable && tab.compare->right_viewport.dirty() &&
        PathEqualsOrWithin(tab.compare->right_path.lexically_normal(), normalized_path)) {
      targets.push_back(DirtyPathTarget{
          .kind = DirtyPathTarget::Kind::CompareTab,
          .tab_index = i,
      });
      continue;
    }

    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.dirty() &&
        (PathEqualsOrWithin(tab.merge->base_path.lexically_normal(), normalized_path) ||
         PathEqualsOrWithin(tab.merge->incoming_path.lexically_normal(), normalized_path) ||
         PathEqualsOrWithin(tab.merge->current_path.lexically_normal(), normalized_path) ||
         PathEqualsOrWithin(tab.merge->output_path.lexically_normal(), normalized_path))) {
      targets.push_back(DirtyPathTarget{
          .kind = DirtyPathTarget::Kind::MergeTab,
          .tab_index = i,
      });
    }
  }
  return targets;
}

std::vector<std::size_t> WorkspaceShell::DirtyTabIndicesForPath(
    const std::filesystem::path& path) const {
  const std::vector<DirtyPathTarget> targets = DirtyPathTargetsForPath(path);
  std::vector<std::size_t> indices;
  indices.reserve(targets.size());
  for (const DirtyPathTarget& target : targets) {
    if (std::find(indices.begin(), indices.end(), target.tab_index) == indices.end()) {
      indices.push_back(target.tab_index);
    }
  }
  return indices;
}

std::vector<std::size_t> WorkspaceShell::AffectedEditorTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (EditorTabReferencesPath(i, path)) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<std::size_t> WorkspaceShell::AffectedCompareTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (PathEqualsOrWithin(tab.compare->path.lexically_normal(), path.lexically_normal())) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<std::size_t> WorkspaceShell::AffectedMergeTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }
    if (PathEqualsOrWithin(tab.merge->base_path.lexically_normal(), path.lexically_normal()) ||
        PathEqualsOrWithin(tab.merge->incoming_path.lexically_normal(), path.lexically_normal()) ||
        PathEqualsOrWithin(tab.merge->current_path.lexically_normal(), path.lexically_normal()) ||
        PathEqualsOrWithin(tab.merge->output_path.lexically_normal(), path.lexically_normal())) {
      indices.push_back(i);
    }
  }
  return indices;
}

bool WorkspaceShell::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                               std::string* blocking_label) const {
  const std::vector<std::size_t> dirty_tabs = DirtyTabIndicesForPath(path);
  if (!dirty_tabs.empty()) {
    if (blocking_label != nullptr) {
      *blocking_label = open_tabs_[dirty_tabs.front()].title;
    }
    return true;
  }
  return false;
}

bool WorkspaceShell::ResolveDirtyTabsForPath(const std::filesystem::path& path,
                                             DirtyPromptState::Kind prompt_kind,
                                             DirtyPathResolution resolution) {
  const std::vector<DirtyPathTarget> dirty_targets = DirtyPathTargetsForPath(path);
  if (dirty_targets.empty()) {
    return true;
  }

  if (resolution == DirtyPathResolution::RequirePrompt) {
    dirty_prompt_visible_ = true;
    dirty_prompt_previous_focus_ = focus_;
    dirty_prompt_state_.kind = prompt_kind;
    dirty_prompt_state_.dirty_tabs = DirtyTabIndicesForPath(path);
    dirty_prompt_state_.dirty_count = dirty_targets.size();
    dirty_prompt_state_.path = path.lexically_normal();
    dirty_prompt_state_.selected_action = 0;
    focus_ = FocusTarget::Overlay;
    return false;
  }

  if (resolution == DirtyPathResolution::Save) {
    bool saved_any = false;
    for (const DirtyPathTarget& target : dirty_targets) {
      if (target.tab_index >= open_tabs_.size()) {
        continue;
      }

      auto& tab = open_tabs_[target.tab_index];
      if (target.kind == DirtyPathTarget::Kind::CompareTab) {
        if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
            !tab.compare->right_viewport.dirty()) {
          continue;
        }
        if (!SaveTab(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }
      if (target.kind == DirtyPathTarget::Kind::MergeTab) {
        if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
          continue;
        }
        if (!tab.merge->result_viewport.dirty()) {
          continue;
        }
        if (!SaveTab(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }

      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      if (target.tab_index == active_tab_index_) {
        SyncActiveEditorTab();
      }

      auto* view_state = FindEditorViewState(*tab.editor_state, target.leaf_id);
      if (view_state == nullptr) {
        continue;
      }

      editor::TextViewport* viewport = &view_state->viewport;
      if (target.tab_index == active_tab_index_ &&
          target.leaf_id == tab.editor_state->active_leaf_id && !view_state->needs_restore) {
        viewport = &text_viewport_;
      }

      if (view_state->needs_restore || viewport->path().empty() || !viewport->dirty()) {
        continue;
      }

      if (!viewport->Save()) {
        return false;
      }
      saved_any = true;

      view_state->restored_path = viewport->path().lexically_normal();
      view_state->restored_cursor_line = viewport->cursor_line();
      view_state->restored_cursor_column = viewport->cursor_column();
      view_state->restored_scroll_line = viewport->scroll_line();
      view_state->restored_horizontal_scroll = viewport->horizontal_scroll();
      view_state->needs_restore = false;
      if (viewport == &text_viewport_) {
        view_state->viewport = text_viewport_;
      }
    }
    if (saved_any) {
      directory_tree_.Refresh();
    }
  }

  return true;
}

void WorkspaceShell::RefreshProjectViewsAfterMutation(
    const std::filesystem::path& preferred_tree_path) {
  RefreshProjectFiles();
  if (!preferred_tree_path.empty() && std::filesystem::exists(preferred_tree_path)) {
    directory_tree_.SelectPath(preferred_tree_path);
  } else if (!project_root_.empty()) {
    directory_tree_.SelectPath(project_root_);
  }
  if (!project_search_query_.empty()) {
    RefreshProjectSearch();
  }
}

void WorkspaceShell::RetargetOpenTabsForRename(const std::filesystem::path& old_path,
                                               const std::filesystem::path& new_path,
                                               bool preserve_unsaved_state) {
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
  }

  std::vector<std::size_t> special_tabs_to_close;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    TabEntry& tab = open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      bool retargeted = false;
      bool close_tab = false;
      for (auto& view : tab.editor_state->views) {
        const std::filesystem::path current_path = EditorViewPath(view);
        if (current_path.empty() || !PathEqualsOrWithin(current_path, old_path)) {
          continue;
        }
        const std::filesystem::path updated_path =
            ReplacePathPrefix(current_path, old_path, new_path).lexically_normal();

        if (!preserve_unsaved_state && !view.needs_restore && view.viewport.dirty()) {
          const std::size_t cursor_line = view.viewport.cursor_line();
          const std::size_t cursor_column = view.viewport.cursor_column();
          const std::size_t scroll_line = view.viewport.scroll_line();
          const std::size_t horizontal_scroll = view.viewport.horizontal_scroll();
          editor::TextViewport reopened_view;
          if (!reopened_view.OpenFile(updated_path)) {
            close_tab = true;
            break;
          }
          ApplyEditorPreferences(reopened_view);
          reopened_view.MoveCursorTo(cursor_line, cursor_column);
          reopened_view.SetScrollLine(scroll_line);
          reopened_view.SetHorizontalScroll(horizontal_scroll);
          view.viewport = std::move(reopened_view);
        } else if (!view.needs_restore) {
          view.viewport.SetPath(updated_path);
        }

        view.restored_path = updated_path;
        if (!view.needs_restore) {
          view.restored_cursor_line = view.viewport.cursor_line();
          view.restored_cursor_column = view.viewport.cursor_column();
          view.restored_scroll_line = view.viewport.scroll_line();
          view.restored_horizontal_scroll = view.viewport.horizontal_scroll();
          view.needs_restore = false;
        }
        retargeted = true;
      }

      if (close_tab) {
        special_tabs_to_close.push_back(i);
        continue;
      }

      if (retargeted) {
        if (i == active_tab_index_) {
          if (const auto* active_view =
                  FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
              active_view != nullptr && !active_view->needs_restore) {
            text_viewport_ = active_view->viewport;
            ApplyEditorPreferences(text_viewport_);
          }
          SyncActiveEditorTabMetadata();
        } else if (const auto* active_view =
                       FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
                   active_view != nullptr) {
          tab.path = EditorViewPath(*active_view);
          tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        PathEqualsOrWithin(tab.compare->path.lexically_normal(), old_path)) {
      const std::filesystem::path updated_path = ReplacePathPrefix(tab.compare->path, old_path, new_path);
      if (preserve_unsaved_state && tab.compare->right_editable && tab.compare->right_viewport.dirty()) {
        tab.compare->path = updated_path.lexically_normal();
        if (tab.compare->right_ref == "WORKTREE" &&
            PathEqualsOrWithin(tab.compare->right_path.lexically_normal(), old_path)) {
          tab.compare->right_path =
              ReplacePathPrefix(tab.compare->right_path, old_path, new_path).lexically_normal();
          tab.compare->right_viewport.SetPath(tab.compare->right_path);
        }
        tab.compare->title = "compare: " + tab.compare->path.filename().string();
        RefreshCompareTabDerivedState(*tab.compare);
        SyncCompareSelectionFromViewport(*tab.compare, false);
        tab.path = tab.compare->path;
        tab.title = tab.compare->title;
        continue;
      }
      CompareTabState updated_compare = *tab.compare;
      updated_compare.path = updated_path.lexically_normal();
      if (updated_compare.right_ref == "WORKTREE" &&
          PathEqualsOrWithin(updated_compare.right_path.lexically_normal(), old_path)) {
        updated_compare.right_path =
            ReplacePathPrefix(updated_compare.right_path, old_path, new_path).lexically_normal();
      }
      auto rebuilt = BuildCompareTabEntry(updated_path, updated_compare);
      if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
        special_tabs_to_close.push_back(i);
        continue;
      }
      tab = std::move(*rebuilt);
      continue;
    }

    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }

    auto update_merge_path = [&](const std::filesystem::path& path) {
      return PathEqualsOrWithin(path.lexically_normal(), old_path)
                 ? ReplacePathPrefix(path, old_path, new_path).lexically_normal()
                 : path.lexically_normal();
    };
    if (!PathEqualsOrWithin(tab.merge->base_path.lexically_normal(), old_path) &&
        !PathEqualsOrWithin(tab.merge->incoming_path.lexically_normal(), old_path) &&
        !PathEqualsOrWithin(tab.merge->current_path.lexically_normal(), old_path) &&
        !PathEqualsOrWithin(tab.merge->output_path.lexically_normal(), old_path)) {
      continue;
    }

    const std::filesystem::path updated_base = update_merge_path(tab.merge->base_path);
    const std::filesystem::path updated_incoming = update_merge_path(tab.merge->incoming_path);
    const std::filesystem::path updated_current = update_merge_path(tab.merge->current_path);
    const std::filesystem::path updated_output = update_merge_path(tab.merge->output_path);

    if (!preserve_unsaved_state) {
      auto rebuilt =
          BuildMergeTabEntry(updated_base, updated_incoming, updated_current, updated_output);
      if (!rebuilt.has_value() || !rebuilt->merge.has_value()) {
        special_tabs_to_close.push_back(i);
        continue;
      }
      auto& rebuilt_merge = rebuilt->merge.value();
      rebuilt_merge.selected_hunk =
          rebuilt_merge.conflicts.empty()
              ? 0
              : std::min(tab.merge->selected_hunk, rebuilt_merge.conflicts.size() - 1);
      rebuilt_merge.scroll_row = tab.merge->scroll_row;
      rebuilt_merge.horizontal_scroll = tab.merge->horizontal_scroll;
      rebuilt_merge.left_divider_fraction = tab.merge->left_divider_fraction;
      rebuilt_merge.right_divider_fraction = tab.merge->right_divider_fraction;
      rebuilt_merge.persistable = tab.merge->persistable;
      rebuilt_merge.result_viewport.SetScrollLine(
          static_cast<std::size_t>(std::max(0, rebuilt_merge.scroll_row)));
      rebuilt_merge.result_viewport.SetHorizontalScroll(rebuilt_merge.horizontal_scroll);
      rebuilt_merge.scroll_row = static_cast<int>(rebuilt_merge.result_viewport.scroll_line());
      rebuilt_merge.horizontal_scroll = rebuilt_merge.result_viewport.horizontal_scroll();
      tab = std::move(*rebuilt);
      continue;
    }

    tab.merge->base_path = updated_base;
    tab.merge->incoming_path = updated_incoming;
    tab.merge->current_path = updated_current;
    tab.merge->output_path = updated_output;
    tab.merge->title = "merge: " + updated_output.filename().string();
    tab.merge->incoming_label = RelativePathLabel(project_root_, updated_incoming);
    tab.merge->result_label = RelativePathLabel(project_root_, updated_output);
    tab.merge->current_label = RelativePathLabel(project_root_, updated_current);
    tab.merge->result_viewport.SetPath(updated_output);
    tab.merge->incoming_initial_syntax_state =
        editor::SyntaxHighlighter::InitialState(updated_output, tab.merge->model.incoming_lines);
    tab.merge->current_initial_syntax_state =
        editor::SyntaxHighlighter::InitialState(updated_output, tab.merge->model.current_lines);
    tab.merge->incoming_current_syntax_state = tab.merge->incoming_initial_syntax_state;
    tab.merge->current_current_syntax_state = tab.merge->current_initial_syntax_state;
    tab.merge->incoming_syntax_rows_tokenized = 0;
    tab.merge->current_syntax_rows_tokenized = 0;
    std::fill(tab.merge->incoming_tokens.begin(), tab.merge->incoming_tokens.end(),
              std::vector<editor::SyntaxTokenKind>{});
    std::fill(tab.merge->current_tokens.begin(), tab.merge->current_tokens.end(),
              std::vector<editor::SyntaxTokenKind>{});
    tab.merge->result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, tab.merge->scroll_row)));
    tab.merge->result_viewport.SetHorizontalScroll(tab.merge->horizontal_scroll);
    tab.merge->scroll_row = static_cast<int>(tab.merge->result_viewport.scroll_line());
    tab.merge->horizontal_scroll = tab.merge->result_viewport.horizontal_scroll();
    tab.path = updated_output;
    tab.title = tab.merge->title;
  }

  std::sort(special_tabs_to_close.rbegin(), special_tabs_to_close.rend());
  for (std::size_t index : special_tabs_to_close) {
    CloseTab(index);
  }

  if (!compare_picker_path_.empty() && PathEqualsOrWithin(compare_picker_path_, old_path)) {
    compare_picker_path_ = ReplacePathPrefix(compare_picker_path_, old_path, new_path);
    if (overlay_visible_ && overlay_mode_ == OverlayMode::CommitPicker) {
      overlay_visible_ = false;
    }
  }
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
  }

  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    auto& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }

    std::vector<std::size_t> removed_leaf_ids;
    for (const auto& view : tab.editor_state->views) {
      const std::filesystem::path current_path = EditorViewPath(view);
      if (!current_path.empty() && PathEqualsOrWithin(current_path, normalized_path)) {
        removed_leaf_ids.push_back(view.leaf_id);
      }
    }
    if (removed_leaf_ids.empty()) {
      continue;
    }

    const auto leaf_removed = [&](std::size_t leaf_id) {
      return std::find(removed_leaf_ids.begin(), removed_leaf_ids.end(), leaf_id) !=
             removed_leaf_ids.end();
    };

    tab.editor_state->views.erase(
        std::remove_if(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                       [&](const auto& view) { return leaf_removed(view.leaf_id); }),
        tab.editor_state->views.end());

    const auto prune_node = [&](auto&& self,
                                std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode>& node)
        -> void {
      if (node == nullptr) {
        return;
      }
      if (node->IsLeaf()) {
        if (leaf_removed(node->leaf_id)) {
          node.reset();
        }
        return;
      }

      for (auto& child : node->children) {
        self(self, child);
      }
      node->children.erase(std::remove(node->children.begin(), node->children.end(), nullptr),
                           node->children.end());
      if (node->children.empty()) {
        node.reset();
        return;
      }
      if (node->children.size() == 1) {
        auto child = std::move(node->children.front());
        child->size_fraction = std::max(node->size_fraction, child->size_fraction);
        node = std::move(child);
      }
    };
    prune_node(prune_node, tab.editor_state->split_root);

    if (tab.editor_state->views.empty() || tab.editor_state->split_root == nullptr) {
      indices.push_back(i);
      continue;
    }

    NormalizeEditorSplitTree(*tab.editor_state);
    if (leaf_removed(tab.editor_state->active_leaf_id)) {
      const std::vector<std::size_t> leaf_order = EditorLeafOrder(*tab.editor_state);
      if (!leaf_order.empty()) {
        tab.editor_state->active_leaf_id = leaf_order.front();
      } else {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
      }
    }

    if (i == active_tab_index_) {
      if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
          active_view != nullptr) {
        text_viewport_ = *active_view;
        ApplyEditorPreferences(text_viewport_);
      }
      SyncActiveEditorTabMetadata();
      ResetCaretBlink();
    } else if (const auto* active_view =
                   FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
               active_view != nullptr) {
      tab.path = EditorViewPath(*active_view);
      tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
    }
  }

  const std::vector<std::size_t> compare_indices = AffectedCompareTabIndices(path);
  const std::vector<std::size_t> merge_indices = AffectedMergeTabIndices(path);
  indices.insert(indices.end(), compare_indices.begin(), compare_indices.end());
  indices.insert(indices.end(), merge_indices.begin(), merge_indices.end());
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  std::sort(indices.rbegin(), indices.rend());
  for (std::size_t index : indices) {
    CloseTab(index);
  }

  if (!compare_picker_path_.empty() && PathEqualsOrWithin(compare_picker_path_, path)) {
    compare_picker_path_.clear();
    compare_picker_query_.clear();
    compare_picker_commits_.clear();
    compare_picker_matches_.clear();
    if (overlay_visible_ && overlay_mode_ == OverlayMode::CommitPicker) {
      overlay_visible_ = false;
    }
  }
}

void WorkspaceShell::ConfirmPromptSurface(DirtyPathResolution resolution) {
  if (!prompt_surface_visible_) {
    return;
  }

  const PromptSurfaceState state = prompt_surface_state_;
  if (state.selected_button == 1) {
    DismissPromptSurface(true);
    return;
  }

  if (state.kind == PromptSurfaceState::Kind::TextInput) {
    if (state.input.empty()) {
      return;
    }

    std::filesystem::path typed_path(state.input);
    if (typed_path.is_absolute()) {
      return;
    }

    std::filesystem::path destination;
    if (state.action == PromptSurfaceState::Action::RenamePath) {
      if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::RenamePath, resolution)) {
        return;
      }
      destination = (state.path.parent_path() / typed_path).lexically_normal();
    } else {
      destination = (state.path / typed_path).lexically_normal();
    }

    if (!PathEqualsOrWithin(destination, project_root_)) {
      return;
    }

    project::FileOperationResult result;
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      result = project::FileOperationService::CreateFile(destination);
    } else if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      result = project::FileOperationService::CreateDirectory(destination);
    } else {
      result = project::FileOperationService::RenamePath(state.path, destination);
    }

    if (!result.ok) {
      return;
    }

    if (dirty_prompt_visible_) {
      DismissDirtyPrompt(false);
    }
    DismissPromptSurface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      OpenFile(result.resulting_path);
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      focus_ = FocusTarget::Sidebar;
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path,
                              resolution != DirtyPathResolution::Discard);
    ClearEditorBlame();
    RefreshProjectViewsAfterMutation(result.resulting_path);
    focus_ = FocusTarget::Sidebar;
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardGitChanges) {
    const bool discarded = DiscardAllGitSidebarEntries();
    DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      focus_ = FocusTarget::Sidebar;
    }
    return;
  }

  if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::DeletePath, resolution)) {
    return;
  }

  const project::FileOperationResult result = project::FileOperationService::TrashPath(state.path);
  if (!result.ok) {
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  if (dirty_prompt_visible_) {
    DismissDirtyPrompt(false);
  }
  DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  ClearEditorBlame();
  RefreshProjectViewsAfterMutation(parent);
  focus_ = FocusTarget::Sidebar;
}

}  // namespace microide::workspace
