#include "workspace/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

void PathMutationCoordinator::RetargetOpenTabsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path,
    bool preserve_unsaved_state) {
  auto& state = CurrentProjectState();
  if (editor_tabs_.ActiveTabIsEditor()) {
    editor_tabs_.SyncActiveEditorTab();
  }

  std::vector<std::size_t> special_tabs_to_close;
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    TabEntry& tab = state.open_tabs[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      bool retargeted = false;
      bool close_tab = false;
      for (auto& view : tab.editor_state->views) {
        const std::filesystem::path current_path = operations_.editor_view_path(view);
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
          operations_.apply_editor_preferences(reopened_view);
          operations_.apply_detected_indent_on_open(reopened_view);
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
        if (i == state.active_tab_index) {
          if (auto* active_view =
                  operations_.find_editor_view_state(*tab.editor_state, tab.editor_state->active_leaf_id);
              active_view != nullptr && !active_view->needs_restore) {
            state.welcome_surface.viewport = active_view->viewport;
            operations_.apply_editor_preferences(state.welcome_surface.viewport);
          }
          operations_.sync_active_editor_tab_metadata();
        } else if (auto* active_view =
                       operations_.find_editor_view_state(*tab.editor_state, tab.editor_state->active_leaf_id);
                   active_view != nullptr) {
          tab.path = operations_.editor_view_path(*active_view);
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
        // Keep selection and syntax-derived state aligned after retargeting a dirty compare tab.
        auto rebuilt = operations_.build_compare_tab_entry(tab.compare->path, *tab.compare);
        if (rebuilt.has_value() && rebuilt->compare.has_value()) {
          rebuilt->compare->right_viewport = tab.compare->right_viewport;
          rebuilt->compare->right_editable = tab.compare->right_editable;
          rebuilt->compare->right_view_active = tab.compare->right_view_active;
          rebuilt->compare->persistable = tab.compare->persistable;
          tab = std::move(*rebuilt);
        }
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
      auto rebuilt = operations_.build_compare_tab_entry(updated_path, updated_compare);
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
          operations_.build_merge_tab_entry(updated_base, updated_incoming, updated_current, updated_output);
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
    tab.merge->incoming_label = RelativePathLabel(state.root, updated_incoming);
    tab.merge->result_label = RelativePathLabel(state.root, updated_output);
    tab.merge->current_label = RelativePathLabel(state.root, updated_current);
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
    editor_tabs_.Close(index);
  }

  if (!state.overlay.workflow.compare_picker.path.empty() &&
      PathEqualsOrWithin(state.overlay.workflow.compare_picker.path, old_path)) {
    state.overlay.workflow.compare_picker.path =
        ReplacePathPrefix(state.overlay.workflow.compare_picker.path, old_path, new_path);
    if (state.overlay.visible && state.overlay.mode == OverlayMode::CommitPicker) {
      // Hide via the focus-safe helper so input does not strand on the dismissed
      // commit picker when its file is renamed/deleted out from under it.
      HideOverlay(state);
    }
  }
}

void PathMutationCoordinator::CloseOpenTabsForPath(const std::filesystem::path& path) {
  auto& state = CurrentProjectState();
  if (editor_tabs_.ActiveTabIsEditor()) {
    editor_tabs_.SyncActiveEditorTab();
  }

  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    auto& tab = state.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }

    std::vector<std::size_t> removed_leaf_ids;
    for (const auto& view : tab.editor_state->views) {
      const std::filesystem::path current_path = operations_.editor_view_path(view);
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

    operations_.normalize_editor_split_tree(*tab.editor_state);
    if (leaf_removed(tab.editor_state->active_leaf_id)) {
      const std::vector<std::size_t> leaf_order = operations_.editor_leaf_order(*tab.editor_state);
      if (!leaf_order.empty()) {
        tab.editor_state->active_leaf_id = leaf_order.front();
      } else {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
      }
    }

    if (i == state.active_tab_index) {
      if (const auto* active_view =
              operations_.find_editor_view(*tab.editor_state, tab.editor_state->active_leaf_id);
          active_view != nullptr) {
        state.welcome_surface.viewport = *active_view;
        operations_.apply_editor_preferences(state.welcome_surface.viewport);
      }
      operations_.sync_active_editor_tab_metadata();
      operations_.reset_caret_blink();
    } else if (auto* active_view =
                   operations_.find_editor_view_state(*tab.editor_state, tab.editor_state->active_leaf_id);
               active_view != nullptr) {
      tab.path = operations_.editor_view_path(*active_view);
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
    editor_tabs_.Close(index);
  }

  if (!state.overlay.workflow.compare_picker.path.empty() &&
      PathEqualsOrWithin(state.overlay.workflow.compare_picker.path, path)) {
    state.overlay.workflow.compare_picker.path.clear();
    state.overlay.workflow.compare_picker.query.SetText("");
    state.overlay.workflow.compare_picker.commits.clear();
    state.overlay.workflow.compare_picker.matches.clear();
    if (state.overlay.visible && state.overlay.mode == OverlayMode::CommitPicker) {
      // Hide via the focus-safe helper so input does not strand on the dismissed
      // commit picker when its file is renamed/deleted out from under it.
      HideOverlay(state);
    }
  }
}

}  // namespace microide::workspace
