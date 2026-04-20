#include "workspace/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

std::vector<PathMutationCoordinator::DirtyPathTarget> PathMutationCoordinator::DirtyPathTargetsForPath(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<DirtyPathTarget> targets;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const WorkspaceShell::TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind == WorkspaceShell::TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        const bool active_live_view =
            i == shell_.active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
            !view.needs_restore;
        const editor::TextViewport& viewport =
            active_live_view ? shell_.text_viewport_ : view.viewport;
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

    if (tab.kind == WorkspaceShell::TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_editable && tab.compare->right_viewport.dirty() &&
        PathEqualsOrWithin(tab.compare->right_path.lexically_normal(), normalized_path)) {
      targets.push_back(DirtyPathTarget{
          .kind = DirtyPathTarget::Kind::CompareTab,
          .tab_index = i,
      });
      continue;
    }

    if (tab.kind == WorkspaceShell::TabEntry::Kind::Merge && tab.merge.has_value() &&
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

std::vector<std::size_t> PathMutationCoordinator::DirtyTabIndicesForPath(
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

std::vector<std::size_t> PathMutationCoordinator::AffectedCompareTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const WorkspaceShell::TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind != WorkspaceShell::TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (PathEqualsOrWithin(tab.compare->path.lexically_normal(), path.lexically_normal())) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<std::size_t> PathMutationCoordinator::AffectedMergeTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const WorkspaceShell::TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind != WorkspaceShell::TabEntry::Kind::Merge || !tab.merge.has_value()) {
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

bool PathMutationCoordinator::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                                        std::string* blocking_label) const {
  const std::vector<std::size_t> dirty_tabs = DirtyTabIndicesForPath(path);
  if (!dirty_tabs.empty()) {
    if (blocking_label != nullptr) {
      *blocking_label = shell_.open_tabs_[dirty_tabs.front()].title;
    }
    return true;
  }
  return false;
}

bool PathMutationCoordinator::ResolveDirtyTabsForPath(
    const std::filesystem::path& path,
    WorkspaceShell::DirtyPromptState::Kind prompt_kind,
    WorkspaceShell::DirtyPathResolution resolution) {
  const std::vector<DirtyPathTarget> dirty_targets = DirtyPathTargetsForPath(path);
  if (dirty_targets.empty()) {
    return true;
  }

  if (resolution == WorkspaceShell::DirtyPathResolution::RequirePrompt) {
    shell_.prompts_.dirty_visible = true;
    shell_.prompts_.dirty_previous_focus = shell_.surface_.focus;
    shell_.prompts_.dirty.kind = prompt_kind;
    shell_.prompts_.dirty.dirty_tabs = DirtyTabIndicesForPath(path);
    shell_.prompts_.dirty.dirty_count = dirty_targets.size();
    shell_.prompts_.dirty.path = path.lexically_normal();
    shell_.prompts_.dirty.selected_action = 0;
    shell_.surface_.focus = WorkspaceShell::FocusTarget::Overlay;
    return false;
  }

  if (resolution == WorkspaceShell::DirtyPathResolution::Save) {
    bool saved_any = false;
    for (const DirtyPathTarget& target : dirty_targets) {
      if (target.tab_index >= shell_.open_tabs_.size()) {
        continue;
      }

      auto& tab = shell_.open_tabs_[target.tab_index];
      if (target.kind == DirtyPathTarget::Kind::CompareTab) {
        if (tab.kind != WorkspaceShell::TabEntry::Kind::Compare || !tab.compare.has_value() ||
            !tab.compare->right_viewport.dirty()) {
          continue;
        }
        if (!shell_.SaveTab(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }
      if (target.kind == DirtyPathTarget::Kind::MergeTab) {
        if (tab.kind != WorkspaceShell::TabEntry::Kind::Merge || !tab.merge.has_value()) {
          continue;
        }
        if (!tab.merge->result_viewport.dirty()) {
          continue;
        }
        if (!shell_.SaveTab(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }

      if (tab.kind != WorkspaceShell::TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      if (target.tab_index == shell_.active_tab_index_) {
        shell_.SyncActiveEditorTab();
      }

      auto* view_state = shell_.FindEditorViewState(*tab.editor_state, target.leaf_id);
      if (view_state == nullptr) {
        continue;
      }

      editor::TextViewport* viewport = &view_state->viewport;
      if (target.tab_index == shell_.active_tab_index_ &&
          target.leaf_id == tab.editor_state->active_leaf_id && !view_state->needs_restore) {
        viewport = &shell_.text_viewport_;
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
      if (viewport == &shell_.text_viewport_) {
        view_state->viewport = shell_.text_viewport_;
      }
    }
    if (saved_any) {
      shell_.directory_tree_.Refresh();
    }
  }

  return true;
}

void PathMutationCoordinator::RefreshDiagnosticsAfterMutation() {
  shell_.RefreshProblemsSidebar();
  shell_.QueueEditorHoverRefresh();
  shell_.RequestEditorSurfaceRedraw();
}

void PathMutationCoordinator::RetargetDiagnosticsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path) {
  if (shell_.diagnostics_store_.RetargetPathPrefix(old_path, new_path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void PathMutationCoordinator::ClearDiagnosticsForPath(const std::filesystem::path& path) {
  if (shell_.diagnostics_store_.ClearPathPrefix(path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void PathMutationCoordinator::RefreshProjectViewsAfterMutation(
    const std::filesystem::path& preferred_tree_path) {
  shell_.RefreshProjectFiles();
  if (!preferred_tree_path.empty() && std::filesystem::exists(preferred_tree_path)) {
    shell_.directory_tree_.SelectPath(preferred_tree_path);
  } else if (!shell_.project_root_.empty()) {
    shell_.directory_tree_.SelectPath(shell_.project_root_);
  }
  shell_.RevealSelectedTreeSidebarLine();
  if (!shell_.overlay_workflow_.project_search.query.empty()) {
    shell_.RefreshProjectSearch();
  }
}

}  // namespace microide::workspace
