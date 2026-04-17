#include "workspace/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "project/FileOperationService.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

WorkspaceShell::PathMutationCoordinator::PathMutationCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

std::vector<WorkspaceShell::PathMutationCoordinator::DirtyPathTarget>
WorkspaceShell::PathMutationCoordinator::DirtyPathTargetsForPath(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<DirtyPathTarget> targets;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
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

std::vector<std::size_t> WorkspaceShell::PathMutationCoordinator::DirtyTabIndicesForPath(
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

std::vector<std::size_t> WorkspaceShell::PathMutationCoordinator::AffectedCompareTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (PathEqualsOrWithin(tab.compare->path.lexically_normal(), path.lexically_normal())) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<std::size_t> WorkspaceShell::PathMutationCoordinator::AffectedMergeTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const TabEntry& tab = shell_.open_tabs_[i];
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

bool WorkspaceShell::PathMutationCoordinator::HasDirtyEditorTabsForPath(
    const std::filesystem::path& path,
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

bool WorkspaceShell::PathMutationCoordinator::ResolveDirtyTabsForPath(
    const std::filesystem::path& path,
    DirtyPromptState::Kind prompt_kind,
    DirtyPathResolution resolution) {
  const std::vector<DirtyPathTarget> dirty_targets = DirtyPathTargetsForPath(path);
  if (dirty_targets.empty()) {
    return true;
  }

  if (resolution == DirtyPathResolution::RequirePrompt) {
    shell_.prompts_.dirty_visible = true;
    shell_.prompts_.dirty_previous_focus = shell_.surface_.focus;
    shell_.prompts_.dirty.kind = prompt_kind;
    shell_.prompts_.dirty.dirty_tabs = DirtyTabIndicesForPath(path);
    shell_.prompts_.dirty.dirty_count = dirty_targets.size();
    shell_.prompts_.dirty.path = path.lexically_normal();
    shell_.prompts_.dirty.selected_action = 0;
    shell_.surface_.focus = FocusTarget::Overlay;
    return false;
  }

  if (resolution == DirtyPathResolution::Save) {
    bool saved_any = false;
    for (const DirtyPathTarget& target : dirty_targets) {
      if (target.tab_index >= shell_.open_tabs_.size()) {
        continue;
      }

      auto& tab = shell_.open_tabs_[target.tab_index];
      if (target.kind == DirtyPathTarget::Kind::CompareTab) {
        if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
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
        if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
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

      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
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

void WorkspaceShell::PathMutationCoordinator::RefreshDiagnosticsAfterMutation() {
  shell_.RefreshProblemsSidebar();
  shell_.QueueEditorHoverRefresh();
  shell_.RequestEditorSurfaceRedraw();
}

void WorkspaceShell::PathMutationCoordinator::RetargetDiagnosticsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path) {
  if (shell_.diagnostics_store_.RetargetPathPrefix(old_path, new_path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void WorkspaceShell::PathMutationCoordinator::ClearDiagnosticsForPath(
    const std::filesystem::path& path) {
  if (shell_.diagnostics_store_.ClearPathPrefix(path)) {
    RefreshDiagnosticsAfterMutation();
  }
}

void WorkspaceShell::PathMutationCoordinator::RefreshProjectViewsAfterMutation(
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

void WorkspaceShell::PathMutationCoordinator::RetargetOpenTabsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path,
    bool preserve_unsaved_state) {
  if (shell_.ActiveTabIsEditor()) {
    shell_.SyncActiveEditorTab();
  }

  std::vector<std::size_t> special_tabs_to_close;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    TabEntry& tab = shell_.open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      bool retargeted = false;
      bool close_tab = false;
      for (auto& view : tab.editor_state->views) {
        const std::filesystem::path current_path = shell_.EditorViewPath(view);
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
          shell_.ApplyEditorPreferences(reopened_view);
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
        if (i == shell_.active_tab_index_) {
          if (const auto* active_view =
                  shell_.FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
              active_view != nullptr && !active_view->needs_restore) {
            shell_.text_viewport_ = active_view->viewport;
            shell_.ApplyEditorPreferences(shell_.text_viewport_);
          }
          shell_.SyncActiveEditorTabMetadata();
        } else if (const auto* active_view =
                       shell_.FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
                   active_view != nullptr) {
          tab.path = shell_.EditorViewPath(*active_view);
          tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        PathEqualsOrWithin(tab.compare->path.lexically_normal(), old_path)) {
      const std::filesystem::path updated_path =
          ReplacePathPrefix(tab.compare->path, old_path, new_path);
      if (preserve_unsaved_state && tab.compare->right_editable &&
          tab.compare->right_viewport.dirty()) {
        tab.compare->path = updated_path.lexically_normal();
        if (tab.compare->right_ref == "WORKTREE" &&
            PathEqualsOrWithin(tab.compare->right_path.lexically_normal(), old_path)) {
          tab.compare->right_path =
              ReplacePathPrefix(tab.compare->right_path, old_path, new_path).lexically_normal();
          tab.compare->right_viewport.SetPath(tab.compare->right_path);
        }
        tab.compare->title = "compare: " + tab.compare->path.filename().string();
        shell_.RefreshCompareTabDerivedState(*tab.compare);
        shell_.SyncCompareSelectionFromViewport(*tab.compare, false);
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
      auto rebuilt = shell_.BuildCompareTabEntry(updated_path, updated_compare);
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
          shell_.BuildMergeTabEntry(updated_base, updated_incoming, updated_current, updated_output);
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
    tab.merge->incoming_label = RelativePathLabel(shell_.project_root_, updated_incoming);
    tab.merge->result_label = RelativePathLabel(shell_.project_root_, updated_output);
    tab.merge->current_label = RelativePathLabel(shell_.project_root_, updated_current);
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
    shell_.CloseTab(index);
  }

  if (!shell_.overlay_workflow_.compare_picker.path.empty() &&
      PathEqualsOrWithin(shell_.overlay_workflow_.compare_picker.path, old_path)) {
    shell_.overlay_workflow_.compare_picker.path =
        ReplacePathPrefix(shell_.overlay_workflow_.compare_picker.path, old_path, new_path);
    if (shell_.surface_.overlay_visible &&
        shell_.surface_.overlay_mode == OverlayMode::CommitPicker) {
      shell_.surface_.overlay_visible = false;
    }
  }
}

void WorkspaceShell::PathMutationCoordinator::CloseOpenTabsForPath(
    const std::filesystem::path& path) {
  if (shell_.ActiveTabIsEditor()) {
    shell_.SyncActiveEditorTab();
  }

  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    auto& tab = shell_.open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }

    std::vector<std::size_t> removed_leaf_ids;
    for (const auto& view : tab.editor_state->views) {
      const std::filesystem::path current_path = shell_.EditorViewPath(view);
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

    shell_.NormalizeEditorSplitTree(*tab.editor_state);
    if (leaf_removed(tab.editor_state->active_leaf_id)) {
      const std::vector<std::size_t> leaf_order = shell_.EditorLeafOrder(*tab.editor_state);
      if (!leaf_order.empty()) {
        tab.editor_state->active_leaf_id = leaf_order.front();
      } else {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
      }
    }

    if (i == shell_.active_tab_index_) {
      if (auto* active_view =
              shell_.FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
          active_view != nullptr) {
        shell_.text_viewport_ = *active_view;
        shell_.ApplyEditorPreferences(shell_.text_viewport_);
      }
      shell_.SyncActiveEditorTabMetadata();
      shell_.ResetCaretBlink();
    } else if (const auto* active_view =
                   shell_.FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
               active_view != nullptr) {
      tab.path = shell_.EditorViewPath(*active_view);
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
    shell_.CloseTab(index);
  }

  if (!shell_.overlay_workflow_.compare_picker.path.empty() &&
      PathEqualsOrWithin(shell_.overlay_workflow_.compare_picker.path, path)) {
    shell_.overlay_workflow_.compare_picker.path.clear();
    shell_.overlay_workflow_.compare_picker.query.clear();
    shell_.overlay_workflow_.compare_picker.commits.clear();
    shell_.overlay_workflow_.compare_picker.matches.clear();
    if (shell_.surface_.overlay_visible &&
        shell_.surface_.overlay_mode == OverlayMode::CommitPicker) {
      shell_.surface_.overlay_visible = false;
    }
  }
}

void WorkspaceShell::PathMutationCoordinator::ConfirmPromptSurface(
    DirtyPathResolution resolution) {
  if (!shell_.prompts_.surface_visible) {
    return;
  }

  const PromptSurfaceState state = shell_.prompts_.surface;
  if (state.selected_button == 1) {
    shell_.DismissPromptSurface(true);
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

    if (!PathEqualsOrWithin(destination, shell_.project_root_)) {
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

    if (shell_.prompts_.dirty_visible) {
      shell_.DismissDirtyPrompt(false);
    }
    shell_.DismissPromptSurface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      shell_.OpenFile(result.resulting_path);
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      shell_.surface_.focus = FocusTarget::Sidebar;
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path,
                              resolution != DirtyPathResolution::Discard);
    RetargetDiagnosticsForRename(state.path, result.resulting_path);
    shell_.ClearEditorBlame();
    RefreshProjectViewsAfterMutation(result.resulting_path);
    shell_.surface_.focus = FocusTarget::Sidebar;
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardGitChanges) {
    const bool discarded = shell_.DiscardAllGitSidebarEntries();
    shell_.DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      shell_.surface_.focus = FocusTarget::Sidebar;
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
  if (shell_.prompts_.dirty_visible) {
    shell_.DismissDirtyPrompt(false);
  }
  shell_.DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  ClearDiagnosticsForPath(state.path);
  shell_.ClearEditorBlame();
  RefreshProjectViewsAfterMutation(parent);
  shell_.surface_.focus = FocusTarget::Sidebar;
}

}  // namespace microide::workspace
