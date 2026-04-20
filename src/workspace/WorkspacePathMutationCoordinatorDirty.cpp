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
  const auto& state = CurrentProjectState();
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const TabEntry& tab = state.open_tabs[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        const bool active_live_view =
            i == state.active_tab_index && view.leaf_id == tab.editor_state->active_leaf_id &&
            !view.needs_restore;
        const editor::TextViewport& viewport =
            active_live_view ? state.text_viewport : view.viewport;
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
  const auto& state = CurrentProjectState();
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const TabEntry& tab = state.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
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
  const auto& state = CurrentProjectState();
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const TabEntry& tab = state.open_tabs[i];
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

bool PathMutationCoordinator::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                                        std::string* blocking_label) const {
  const std::vector<std::size_t> dirty_tabs = DirtyTabIndicesForPath(path);
  if (!dirty_tabs.empty()) {
    if (blocking_label != nullptr) {
      *blocking_label = CurrentProjectState().open_tabs[dirty_tabs.front()].title;
    }
    return true;
  }
  return false;
}

bool PathMutationCoordinator::ResolveDirtyTabsForPath(
    const std::filesystem::path& path,
    DirtyPromptState::Kind prompt_kind,
    WorkspaceShell::DirtyPathResolution resolution) {
  auto& state = CurrentProjectState();
  const std::vector<DirtyPathTarget> dirty_targets = DirtyPathTargetsForPath(path);
  if (dirty_targets.empty()) {
    return true;
  }

  if (resolution == WorkspaceShell::DirtyPathResolution::RequirePrompt) {
    context_.prompts.dirty_visible = true;
    context_.prompts.dirty_previous_focus = state.surface.focus;
    context_.prompts.dirty.kind = prompt_kind;
    context_.prompts.dirty.dirty_tabs = DirtyTabIndicesForPath(path);
    context_.prompts.dirty.dirty_count = dirty_targets.size();
    context_.prompts.dirty.path = path.lexically_normal();
    context_.prompts.dirty.selected_action = 0;
    state.surface.focus = FocusTarget::Overlay;
    return false;
  }

  if (resolution == WorkspaceShell::DirtyPathResolution::Save) {
    bool saved_any = false;
    for (const DirtyPathTarget& target : dirty_targets) {
      if (target.tab_index >= state.open_tabs.size()) {
        continue;
      }

      auto& tab = state.open_tabs[target.tab_index];
      if (target.kind == DirtyPathTarget::Kind::CompareTab) {
        if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
            !tab.compare->right_viewport.dirty()) {
          continue;
        }
        if (!operations_.save_tab(target.tab_index)) {
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
        if (!operations_.save_tab(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }

      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      if (target.tab_index == state.active_tab_index) {
        operations_.sync_active_editor_tab();
      }

      auto* view_state = operations_.find_editor_view_state(*tab.editor_state, target.leaf_id);
      if (view_state == nullptr) {
        continue;
      }

      editor::TextViewport* viewport = &view_state->viewport;
      if (target.tab_index == state.active_tab_index &&
          target.leaf_id == tab.editor_state->active_leaf_id && !view_state->needs_restore) {
        viewport = &state.text_viewport;
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
      if (viewport == &state.text_viewport) {
        view_state->viewport = state.text_viewport;
      }
    }
    if (saved_any) {
      state.directory_tree.Refresh();
    }
  }

  return true;
}

void PathMutationCoordinator::RefreshDiagnosticsAfterMutation() {
  operations_.refresh_problems_sidebar();
  operations_.queue_editor_hover_refresh();
  operations_.request_editor_surface_redraw();
}

void PathMutationCoordinator::RetargetDiagnosticsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path) {
  if (CurrentProjectState().diagnostics_store.RetargetPathPrefix(old_path, new_path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void PathMutationCoordinator::ClearDiagnosticsForPath(const std::filesystem::path& path) {
  if (CurrentProjectState().diagnostics_store.ClearPathPrefix(path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void PathMutationCoordinator::RefreshProjectViewsAfterMutation(
    const std::filesystem::path& preferred_tree_path) {
  operations_.refresh_project_files();
  auto& state = CurrentProjectState();
  if (!preferred_tree_path.empty() && std::filesystem::exists(preferred_tree_path)) {
    state.directory_tree.SelectPath(preferred_tree_path);
  } else if (!state.root.empty()) {
    state.directory_tree.SelectPath(state.root);
  }
  operations_.reveal_selected_tree_sidebar_line();
  if (!state.overlay.workflow.project_search.query.empty()) {
    operations_.refresh_project_search();
  }
}

}  // namespace microide::workspace
