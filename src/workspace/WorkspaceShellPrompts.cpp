#include "workspace/WorkspaceShell.h"

#include <algorithm>

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
    LogMessage(prompt.kind == DirtyPromptState::Kind::Quit ? "Quit cancelled" : "Close cancelled");
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
      LogMessage("Save failed");
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
        LogMessage("Failed to switch project");
        return;
      }
    }
    if (prompt.selected_action == 0) {
      for (std::size_t index : prompt.dirty_tabs) {
        if (!SaveTab(index)) {
          LogMessage("Save failed");
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
          LogMessage("Save failed");
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
  LogMessage(prompt.selected_action == 0 ? "Quit confirmed" : "Quit without saving");
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
               ? "Save the dirty tab before " + action + label + "?"
               : "Save the " + std::to_string(dirty_prompt_state_.dirty_count) +
                     " dirty tabs before " + action + label + "?";
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

std::vector<std::size_t> WorkspaceShell::DirtyTabIndicesForPath(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (EditorTabHasDirtyPath(i, path)) {
      indices.push_back(i);
      continue;
    }

    const TabEntry& tab = open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.dirty() &&
        (PathEqualsOrWithin(tab.merge->base_path.lexically_normal(), path.lexically_normal()) ||
         PathEqualsOrWithin(tab.merge->incoming_path.lexically_normal(), path.lexically_normal()) ||
         PathEqualsOrWithin(tab.merge->current_path.lexically_normal(), path.lexically_normal()) ||
         PathEqualsOrWithin(tab.merge->output_path.lexically_normal(), path.lexically_normal()))) {
      indices.push_back(i);
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
  const std::vector<std::size_t> dirty_tabs = DirtyTabIndicesForPath(path);
  if (dirty_tabs.empty()) {
    return true;
  }

  if (resolution == DirtyPathResolution::RequirePrompt) {
    dirty_prompt_visible_ = true;
    dirty_prompt_previous_focus_ = focus_;
    dirty_prompt_state_.kind = prompt_kind;
    dirty_prompt_state_.dirty_tabs = dirty_tabs;
    dirty_prompt_state_.dirty_count = dirty_tabs.size();
    dirty_prompt_state_.path = path.lexically_normal();
    dirty_prompt_state_.selected_action = 0;
    focus_ = FocusTarget::Overlay;
    return false;
  }

  if (resolution == DirtyPathResolution::Save) {
    for (std::size_t index : dirty_tabs) {
      if (!SaveTab(index)) {
        LogMessage("Save failed");
        return false;
      }
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
      auto rebuilt = BuildCompareTabEntry(updated_path, *tab.compare);
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

    auto rebuilt = BuildMergeTabEntry(update_merge_path(tab.merge->base_path),
                                      update_merge_path(tab.merge->incoming_path),
                                      update_merge_path(tab.merge->current_path),
                                      update_merge_path(tab.merge->output_path));
    if (!rebuilt.has_value() || !rebuilt->merge.has_value()) {
      special_tabs_to_close.push_back(i);
      continue;
    }
    auto& rebuilt_merge = rebuilt->merge.value();
    rebuilt_merge.selected_hunk = rebuilt_merge.model.hunks.empty()
                                      ? 0
                                      : std::min(tab.merge->selected_hunk,
                                                 rebuilt_merge.model.hunks.size() - 1);
    if (preserve_unsaved_state || !tab.merge->result_viewport.dirty()) {
      for (std::size_t hunk_index = 0;
           hunk_index < rebuilt_merge.model.hunks.size() &&
           hunk_index < tab.merge->model.hunks.size();
           ++hunk_index) {
        rebuilt_merge.model.hunks[hunk_index].choice = tab.merge->model.hunks[hunk_index].choice;
      }
      RefreshMergeTabDerivedState(rebuilt_merge);
    }
    rebuilt_merge.scroll_row = tab.merge->scroll_row;
    rebuilt_merge.horizontal_scroll = tab.merge->horizontal_scroll;
    rebuilt_merge.persistable = tab.merge->persistable;
    tab = std::move(*rebuilt);
  }

  std::sort(special_tabs_to_close.rbegin(), special_tabs_to_close.rend());
  for (std::size_t index : special_tabs_to_close) {
    CloseTab(index);
  }

  if (!compare_picker_path_.empty() && PathEqualsOrWithin(compare_picker_path_, old_path)) {
    compare_picker_path_ = ReplacePathPrefix(compare_picker_path_, old_path, new_path);
    if (overlay_visible_ && overlay_mode_ == OverlayMode::CommitPicker) {
      overlay_visible_ = false;
      LogMessage("Compare picker closed after rename");
    }
  }
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
  }

  std::vector<std::size_t> indices = AffectedEditorTabIndices(path);
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
      LogMessage("Compare picker closed after delete");
    }
  }
}

void WorkspaceShell::ConfirmPromptSurface(DirtyPathResolution resolution) {
  if (!prompt_surface_visible_) {
    return;
  }

  const PromptSurfaceState state = prompt_surface_state_;
  if (state.selected_button == 1) {
    const std::string title = PromptSurfaceTitle();
    DismissPromptSurface(true);
    LogMessage(title + " cancelled");
    return;
  }

  if (state.kind == PromptSurfaceState::Kind::TextInput) {
    if (state.input.empty()) {
      LogMessage("A path is required");
      return;
    }

    std::filesystem::path typed_path(state.input);
    if (typed_path.is_absolute()) {
      LogMessage("Enter a project-relative path");
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
      LogMessage("Path must stay inside the current project");
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
      LogMessage(result.error_message);
      return;
    }

    if (dirty_prompt_visible_) {
      DismissDirtyPrompt(false);
    }
    DismissPromptSurface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      OpenFile(result.resulting_path);
      LogMessage("Created file: " + RelativePathLabel(project_root_, result.resulting_path));
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      focus_ = FocusTarget::Sidebar;
      LogMessage("Created folder: " + RelativePathLabel(project_root_, result.resulting_path));
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path,
                              resolution != DirtyPathResolution::Discard);
    RefreshProjectViewsAfterMutation(result.resulting_path);
    focus_ = FocusTarget::Sidebar;
    LogMessage("Renamed: " + RelativePathLabel(project_root_, result.resulting_path));
    return;
  }

  if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::DeletePath, resolution)) {
    return;
  }

  const project::FileOperationResult result = project::FileOperationService::TrashPath(state.path);
  if (!result.ok) {
    LogMessage(result.error_message);
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  if (dirty_prompt_visible_) {
    DismissDirtyPrompt(false);
  }
  DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  RefreshProjectViewsAfterMutation(parent);
  focus_ = FocusTarget::Sidebar;
  LogMessage("Moved to trash: " + RelativePathLabel(project_root_, state.path));
}

}  // namespace microide::workspace
