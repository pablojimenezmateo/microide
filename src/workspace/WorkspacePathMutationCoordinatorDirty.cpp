#include "workspace/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "workspace/PromptSurfaceService.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

std::vector<PathMutationCoordinator::DirtyPathTarget> PathMutationCoordinator::DirtyPathTargetsForPath(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<DirtyPathTarget> targets;
  const auto& state = CurrentProjectState();
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    const TabEntry& tab = state.focused_group().open_tabs[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      const auto& editor_state = *tab.editor_state;
      if (!editor_state.needs_restore && !editor_state.viewport.path().empty() &&
          editor_state.viewport.dirty() &&
          PathEqualsOrWithin(editor_state.viewport.path().lexically_normal(), normalized_path)) {
        targets.push_back(DirtyPathTarget{
            .kind = DirtyPathTarget::Kind::EditorView,
            .tab_index = i,
        });
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
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    const TabEntry& tab = state.focused_group().open_tabs[i];
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
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    const TabEntry& tab = state.focused_group().open_tabs[i];
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
      *blocking_label = CurrentProjectState().focused_group().open_tabs[dirty_tabs.front()].title;
    }
    return true;
  }
  return false;
}

bool PathMutationCoordinator::ResolveDirtyTabsForPath(
    const std::filesystem::path& path,
    DirtyPromptState::Kind prompt_kind,
    DirtyPathResolution resolution) {
  auto& state = CurrentProjectState();
  const std::vector<DirtyPathTarget> dirty_targets = DirtyPathTargetsForPath(path);
  if (dirty_targets.empty()) {
    return true;
  }

  if (resolution == DirtyPathResolution::RequirePrompt) {
    prompt_surfaces_.ShowDirtyPathPrompt(prompt_kind, DirtyTabIndicesForPath(path),
                                         dirty_targets.size(), path);
    return false;
  }

  if (resolution == DirtyPathResolution::Save) {
    bool saved_any = false;
    for (const DirtyPathTarget& target : dirty_targets) {
      if (target.tab_index >= state.focused_group().open_tabs.size()) {
        continue;
      }

      auto& tab = state.focused_group().open_tabs[target.tab_index];
      if (target.kind == DirtyPathTarget::Kind::CompareTab) {
        if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
            !tab.compare->right_viewport.dirty()) {
          continue;
        }
        if (!editor_tabs_.Save(target.tab_index)) {
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
        if (!editor_tabs_.Save(target.tab_index)) {
          return false;
        }
        saved_any = true;
        continue;
      }

      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }

      if (target.tab_index == state.focused_group().active_tab_index) {
        editor_tabs_.SyncActiveEditorTab();
      }

      auto& editor_state = *tab.editor_state;
      editor::TextViewport* viewport = &editor_state.viewport;
      if (editor_state.needs_restore || viewport->path().empty() || !viewport->dirty()) {
        continue;
      }

      if (!viewport->Save()) {
        return false;
      }
      saved_any = true;

      editor_state.restored_path = viewport->path().lexically_normal();
      editor_state.restored_cursor_line = viewport->cursor_line();
      editor_state.restored_cursor_column = viewport->cursor_column();
      editor_state.restored_scroll_line = viewport->scroll_line();
      editor_state.restored_horizontal_scroll = viewport->horizontal_scroll();
      editor_state.needs_restore = false;
    }
    if (saved_any) {
      state.directory_tree.Refresh();
      if (operations_.request_automatic_git_sidebar_refresh) {
        operations_.request_automatic_git_sidebar_refresh();
      }
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

void PathMutationCoordinator::RetargetPluginDecorationsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path) {
  auto& state = CurrentProjectState();
  // Never allocate the bundle just to retarget nothing: if no plugin presentation is
  // live there are no decorations to move.
  if (state.plugin_presentation_if_present() == nullptr) {
    return;
  }
  if (state.EnsurePluginPresentation().decorations.RetargetPathPrefix(old_path, new_path)) {
    operations_.request_editor_surface_redraw();
  }
  // Retarget can only shrink/relabel keys, never empty the store, so a release check is
  // unnecessary here — but harmless and keeps the zero-footprint invariant tight.
  state.MaybeReleasePluginPresentation();
}

void PathMutationCoordinator::ClearPluginDecorationsForPath(const std::filesystem::path& path) {
  auto& state = CurrentProjectState();
  if (state.plugin_presentation_if_present() == nullptr) {
    return;
  }
  if (state.EnsurePluginPresentation().decorations.ClearPathPrefix(path)) {
    operations_.request_editor_surface_redraw();
  }
  // Deleting the last decorated path can drain the bundle; release it back to zero
  // footprint (called after the redraw request so no reader holds a store pointer).
  state.MaybeReleasePluginPresentation();
}

void PathMutationCoordinator::RefreshProjectViewsAfterMutation(
    const std::filesystem::path& preferred_tree_path) {
  operations_.refresh_project_files();
  auto& state = CurrentProjectState();
  std::error_code preferred_exists_error;
  if (!preferred_tree_path.empty() &&
      std::filesystem::exists(preferred_tree_path, preferred_exists_error) &&
      !preferred_exists_error) {
    state.directory_tree.SelectPath(preferred_tree_path);
  } else if (!state.root.empty()) {
    state.directory_tree.SelectPath(state.root);
  }
  operations_.reveal_selected_tree_sidebar_line();
  if (!state.overlay.workflow.project_search.query.text().empty()) {
    operations_.refresh_project_search();
  }
}

}  // namespace microide::workspace
