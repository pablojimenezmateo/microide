#include "workspace/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "util/PathMatch.h"
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
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    TabEntry& tab = state.focused_group().open_tabs[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      auto& editor_state = *tab.editor_state;
      bool retargeted = false;
      bool close_tab = false;
      const std::filesystem::path current_path = operations_.editor_view_path(editor_state);
      if (!current_path.empty() && PathEqualsOrWithin(current_path, old_path)) {
        const std::filesystem::path updated_path =
            util::ReplacePathPrefix(current_path, old_path, new_path).lexically_normal();

        if (!preserve_unsaved_state && !editor_state.needs_restore &&
            editor_state.viewport.dirty()) {
          const std::size_t cursor_line = editor_state.viewport.cursor_line();
          const std::size_t cursor_column = editor_state.viewport.cursor_column();
          const std::size_t scroll_line = editor_state.viewport.scroll_line();
          const std::size_t horizontal_scroll = editor_state.viewport.horizontal_scroll();
          editor::TextViewport reopened_view;
          if (!reopened_view.OpenFile(updated_path)) {
            close_tab = true;
          } else {
            operations_.apply_editor_preferences(reopened_view);
            operations_.apply_detected_indent_on_open(reopened_view);
            reopened_view.MoveCursorTo(cursor_line, cursor_column);
            reopened_view.SetScrollLine(scroll_line);
            reopened_view.SetHorizontalScroll(horizontal_scroll);
            editor_state.viewport = std::move(reopened_view);
          }
        } else if (!editor_state.needs_restore) {
          editor_state.viewport.SetPath(updated_path);
        }

        if (!close_tab) {
          editor_state.restored_path = updated_path;
          if (!editor_state.needs_restore) {
            editor_state.restored_cursor_line = editor_state.viewport.cursor_line();
            editor_state.restored_cursor_column = editor_state.viewport.cursor_column();
            editor_state.restored_scroll_line = editor_state.viewport.scroll_line();
            editor_state.restored_horizontal_scroll = editor_state.viewport.horizontal_scroll();
            editor_state.needs_restore = false;
          }
          retargeted = true;
        }
      }

      if (close_tab) {
        special_tabs_to_close.push_back(i);
        continue;
      }

      if (retargeted) {
        if (i == state.focused_group().active_tab_index) {
          operations_.sync_active_editor_tab_metadata();
        } else {
          tab.path = operations_.editor_view_path(editor_state);
          tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        PathEqualsOrWithin(tab.compare->path.lexically_normal(), old_path)) {
      const std::filesystem::path updated_path =
          util::ReplacePathPrefix(tab.compare->path, old_path, new_path);
      if (preserve_unsaved_state && tab.compare->right_editable &&
          tab.compare->right_viewport.dirty()) {
        tab.compare->path = updated_path.lexically_normal();
        if (tab.compare->right_ref == "WORKTREE" &&
            PathEqualsOrWithin(tab.compare->right_path.lexically_normal(), old_path)) {
          tab.compare->right_path =
              util::ReplacePathPrefix(tab.compare->right_path, old_path, new_path).lexically_normal();
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
            util::ReplacePathPrefix(updated_compare.right_path, old_path, new_path).lexically_normal();
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
                 ? util::ReplacePathPrefix(path, old_path, new_path).lexically_normal()
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

  // Retarget matching editor tabs in the NON-focused split groups too — BEFORE the
  // focused close below, so a focused-group collapse (dirty-reopen-fail) that promotes a
  // background group to focused does not skip it here. A split view of the renamed file
  // otherwise keeps a stale path, and — now that autosave/save-all flush dirty tabs
  // across every group — would write the buffer back to the OLD path, resurrecting the
  // pre-rename file. SetPath preserves any unsaved edits and just redirects the write
  // target. (Compare/merge chrome in a background group is display-only and self-heals on
  // the next refresh, so only editor tabs are handled.)
  for (std::size_t gi = 0; gi < state.editor_groups.size(); ++gi) {
    if (gi == state.focused_group_index) {
      continue;
    }
    for (TabEntry& tab : state.editor_groups[gi].open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      if (editor_state.needs_restore) {
        // Restore-pending tab: fix the deferred-open target path.
        if (!editor_state.restored_path.empty() &&
            PathEqualsOrWithin(editor_state.restored_path.lexically_normal(), old_path)) {
          editor_state.restored_path =
              util::ReplacePathPrefix(editor_state.restored_path, old_path, new_path)
                  .lexically_normal();
          tab.path = editor_state.restored_path;
          tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
        }
        continue;
      }
      const std::filesystem::path current_path = operations_.editor_view_path(editor_state);
      if (current_path.empty() || !PathEqualsOrWithin(current_path, old_path)) {
        continue;
      }
      const std::filesystem::path updated_path =
          util::ReplacePathPrefix(current_path, old_path, new_path).lexically_normal();
      // Honor the user's Discard choice in background groups too, mirroring the focused
      // path: when the rename discards unsaved edits, reopen fresh from disk at the new
      // path so the "discarded" buffer is not later written back by all-groups autosave.
      // If the reopen fails, fall back to SetPath (keep the buffer, just retarget) so no
      // data is lost and nothing writes back to the old path.
      if (!preserve_unsaved_state && editor_state.viewport.dirty()) {
        const std::size_t cursor_line = editor_state.viewport.cursor_line();
        const std::size_t cursor_column = editor_state.viewport.cursor_column();
        const std::size_t scroll_line = editor_state.viewport.scroll_line();
        const std::size_t horizontal_scroll = editor_state.viewport.horizontal_scroll();
        editor::TextViewport reopened_view;
        if (reopened_view.OpenFile(updated_path)) {
          operations_.apply_editor_preferences(reopened_view);
          operations_.apply_detected_indent_on_open(reopened_view);
          reopened_view.MoveCursorTo(cursor_line, cursor_column);
          reopened_view.SetScrollLine(scroll_line);
          reopened_view.SetHorizontalScroll(horizontal_scroll);
          editor_state.viewport = std::move(reopened_view);
        } else {
          editor_state.viewport.SetPath(updated_path);
        }
      } else {
        editor_state.viewport.SetPath(updated_path);
      }
      editor_state.restored_path = updated_path;
      editor_state.restored_cursor_line = editor_state.viewport.cursor_line();
      editor_state.restored_cursor_column = editor_state.viewport.cursor_column();
      editor_state.restored_scroll_line = editor_state.viewport.scroll_line();
      editor_state.restored_horizontal_scroll = editor_state.viewport.horizontal_scroll();
      tab.path = updated_path;
      tab.title = updated_path.filename().string();
    }
  }

  // Focused-group tabs that could not be reopened at the new path are closed last (this
  // may collapse the focused group; the non-focused retargets above already ran).
  std::sort(special_tabs_to_close.rbegin(), special_tabs_to_close.rend());
  for (std::size_t index : special_tabs_to_close) {
    editor_tabs_.Close(index);
  }

  if (!state.overlay.workflow.compare_picker.path.empty() &&
      PathEqualsOrWithin(state.overlay.workflow.compare_picker.path, old_path)) {
    state.overlay.workflow.compare_picker.path =
        util::ReplacePathPrefix(state.overlay.workflow.compare_picker.path, old_path, new_path);
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
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    auto& tab = state.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }

    const std::filesystem::path current_path = operations_.editor_view_path(*tab.editor_state);
    if (!current_path.empty() && PathEqualsOrWithin(current_path, normalized_path)) {
      indices.push_back(i);
    }
  }

  const std::vector<std::size_t> compare_indices = AffectedCompareTabIndices(path);
  const std::vector<std::size_t> merge_indices = AffectedMergeTabIndices(path);
  indices.insert(indices.end(), compare_indices.begin(), compare_indices.end());
  indices.insert(indices.end(), merge_indices.begin(), merge_indices.end());
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  std::sort(indices.rbegin(), indices.rend());

  // Close matching editor tabs in the NON-focused split groups FIRST: a deleted file's
  // split view otherwise lingers on the defunct path and (with all-groups autosave)
  // would recreate the just-deleted file on the next flush. Doing this before the
  // focused close means a focused-group collapse cannot promote a still-affected
  // background group to focused and skip it. `focused_group_index` can shift as a
  // background group collapses, so re-read it and iterate the survivors from the back.
  // Compare/merge tabs in a background group are left to self-heal.
  for (std::size_t gi = state.editor_groups.size(); gi-- > 0;) {
    if (gi == state.focused_group_index || gi >= state.editor_groups.size()) {
      continue;
    }
    std::vector<std::size_t> group_indices;
    const auto& tabs = state.editor_groups[gi].open_tabs;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      const TabEntry& tab = tabs[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const std::filesystem::path current_path = operations_.editor_view_path(*tab.editor_state);
      if (!current_path.empty() && PathEqualsOrWithin(current_path, normalized_path)) {
        group_indices.push_back(i);
      }
    }
    std::sort(group_indices.rbegin(), group_indices.rend());
    for (std::size_t index : group_indices) {
      editor_tabs_.CloseGroupTab(gi, index);
    }
  }

  // Now the focused group (indices were captured against it above; non-focused collapses
  // only re-home its index, leaving its own open_tabs — and these indices — valid).
  for (std::size_t index : indices) {
    editor_tabs_.Close(index);
  }

  if (!state.overlay.workflow.compare_picker.path.empty() &&
      PathEqualsOrWithin(state.overlay.workflow.compare_picker.path, path)) {
    state.overlay.workflow.compare_picker.path.clear();
    state.overlay.workflow.compare_picker.query.SetText("");
    state.overlay.workflow.compare_picker.items.clear();
    state.overlay.workflow.compare_picker.matches.clear();
    if (state.overlay.visible && state.overlay.mode == OverlayMode::CommitPicker) {
      // Hide via the focus-safe helper so input does not strand on the dismissed
      // commit picker when its file is renamed/deleted out from under it.
      HideOverlay(state);
    }
  }
}

}  // namespace microide::workspace
