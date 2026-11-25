#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "util/PathMatch.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

PathMutationCoordinator::SpecialTabRetargetOutcome
PathMutationCoordinator::RetargetSpecialTabForRename(TabEntry& tab,
                                                     const std::filesystem::path& old_path,
                                                     const std::filesystem::path& new_path,
                                                     bool preserve_unsaved_state) {
  auto& state = CurrentProjectState();

  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
      util::PathEqualsOrWithinNormalized(tab.compare->path, old_path)) {
    CompareTabState& compare = *tab.compare;
    const auto retarget_in_place = [&] {
      compare.path = util::NormalizedPath(util::ReplacePathPrefix(compare.path, old_path, new_path));
      if (compare.right_ref == "WORKTREE" && util::PathEqualsOrWithinNormalized(compare.right_path, old_path)) {
        compare.right_path = util::ReplacePathPrefix(compare.right_path, old_path, new_path).lexically_normal();
        compare.right_viewport.SetPath(compare.right_path);
      }
      compare.title = "compare: " + compare.path.filename().string();
      tab.path = compare.path;
      tab.title = compare.title;
    };
    if (compare.load_pending) {
      retarget_in_place();
      return SpecialTabRetargetOutcome::Retargeted;
    }
    const bool keep_unsaved = preserve_unsaved_state && compare.right_editable && compare.right_viewport.dirty();
    if (!keep_unsaved && CompareRefreshReadsGit(compare)) {
      retarget_in_place();
      compare.refresh_owed = true;
      return SpecialTabRetargetOutcome::Retargeted;
    }
    retarget_in_place();
    auto rebuilt = operations_.build_compare_tab_entry(compare.path, compare);
    if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
      return keep_unsaved ? SpecialTabRetargetOutcome::Retargeted : SpecialTabRetargetOutcome::MustClose;
    }
    if (keep_unsaved) {
      rebuilt->compare->right_viewport = compare.right_viewport;
      rebuilt->compare->right_editable = compare.right_editable;
      rebuilt->compare->right_view_active = compare.right_view_active;
      rebuilt->compare->persistable = compare.persistable;
    }
    tab = std::move(*rebuilt);
    return SpecialTabRetargetOutcome::Retargeted;
  }

  if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
    return SpecialTabRetargetOutcome::Unaffected;
  }

  auto update_merge_path = [&](const std::filesystem::path& path) {
    return util::PathEqualsOrWithinNormalized(path, old_path)
               ? util::ReplacePathPrefix(path, old_path, new_path).lexically_normal()
               : util::NormalizedPath(path);
  };
  if (!util::PathEqualsOrWithinNormalized(tab.merge->base_path, old_path) &&
      !util::PathEqualsOrWithinNormalized(tab.merge->incoming_path, old_path) &&
      !util::PathEqualsOrWithinNormalized(tab.merge->current_path, old_path) &&
      !util::PathEqualsOrWithinNormalized(tab.merge->output_path, old_path)) {
    return SpecialTabRetargetOutcome::Unaffected;
  }

  const std::filesystem::path updated_base = update_merge_path(tab.merge->base_path);
  const std::filesystem::path updated_incoming = update_merge_path(tab.merge->incoming_path);
  const std::filesystem::path updated_current = update_merge_path(tab.merge->current_path);
  const std::filesystem::path updated_output = update_merge_path(tab.merge->output_path);

  if (!preserve_unsaved_state) {
    auto rebuilt =
        operations_.build_merge_tab_entry(updated_base, updated_incoming, updated_current, updated_output);
    if (!rebuilt.has_value() || !rebuilt->merge.has_value()) {
      return SpecialTabRetargetOutcome::MustClose;
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
    return SpecialTabRetargetOutcome::Retargeted;
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
  tab.merge->incoming_token_window.Reset(
      tab.merge->model.incoming_lines.size(),
      editor::SyntaxHighlighter::InitialState(updated_output, tab.merge->model.incoming_lines));
  tab.merge->current_token_window.Reset(
      tab.merge->model.current_lines.size(),
      editor::SyntaxHighlighter::InitialState(updated_output, tab.merge->model.current_lines));
  tab.merge->result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, tab.merge->scroll_row)));
  tab.merge->result_viewport.SetHorizontalScroll(tab.merge->horizontal_scroll);
  tab.merge->scroll_row = static_cast<int>(tab.merge->result_viewport.scroll_line());
  tab.merge->horizontal_scroll = tab.merge->result_viewport.horizontal_scroll();
  tab.path = updated_output;
  tab.title = tab.merge->title;
  return SpecialTabRetargetOutcome::Retargeted;
}

void PathMutationCoordinator::RetargetOpenTabsForRename(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path,
    bool preserve_unsaved_state) {
  auto& state = CurrentProjectState();
  std::unordered_map<std::string, editor::TextViewport> reopened_by_path;
  const auto reopen_discarded_view =
      [&](const std::filesystem::path& updated_path) -> const editor::TextViewport* {
    auto it = reopened_by_path.find(updated_path.generic_string());
    if (it == reopened_by_path.end()) {
      editor::TextViewport reopened_view;
      if (!reopened_view.OpenFile(updated_path)) {
        return nullptr;
      }
      operations_.apply_editor_preferences(reopened_view);
      operations_.apply_detected_indent_on_open(reopened_view);
      it = reopened_by_path.emplace(updated_path.generic_string(), std::move(reopened_view)).first;
    }
    return &it->second;
  };
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
      const std::filesystem::path current_path =
          tab.path.empty() ? operations_.editor_view_path(editor_state) : tab.path.lexically_normal();
      if (!current_path.empty() && PathEqualsOrWithin(current_path, old_path)) {
        const std::filesystem::path updated_path =
            util::ReplacePathPrefix(current_path, old_path, new_path).lexically_normal();

        if (!preserve_unsaved_state && !editor_state.content_pending() &&
            editor_state.viewport.dirty()) {
          const std::size_t cursor_line = editor_state.viewport.cursor_line();
          const std::size_t cursor_column = editor_state.viewport.cursor_column();
          const std::size_t scroll_line = editor_state.viewport.scroll_line();
          const std::size_t horizontal_scroll = editor_state.viewport.horizontal_scroll();
          if (const editor::TextViewport* reopened_view = reopen_discarded_view(updated_path);
              reopened_view == nullptr) {
            close_tab = true;
          } else {
            editor_state.viewport = *reopened_view;
            editor_state.viewport.MoveCursorTo(cursor_line, cursor_column);
            editor_state.viewport.SetScrollLine(scroll_line);
            editor_state.viewport.SetHorizontalScroll(horizontal_scroll);
          }
        } else if (!editor_state.content_pending()) {
          editor_state.viewport.SetPath(updated_path);
        }

        if (!close_tab) {
          editor_state.restore.path = updated_path;
          if (!editor_state.content_pending()) {
            editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
            editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
            editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
            editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
            editor_state.content = EditorTabState::Content::Ready;
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
          tab.title = TabTitleForPath(tab.path, tab.untitled_number);
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Editor && tab.deferred_handle.has_value()) {
      auto& handle = *tab.deferred_handle;
      if (!handle.path.empty() && util::PathEqualsOrWithinNormalized(handle.path, old_path)) {
        handle.path = util::ReplacePathPrefix(handle.path, old_path, new_path).lexically_normal();
        tab.path = handle.path;
        tab.title = TabTitleForPath(tab.path, tab.untitled_number);
      }
      continue;
    }

    if (RetargetSpecialTabForRename(tab, old_path, new_path, preserve_unsaved_state) ==
        SpecialTabRetargetOutcome::MustClose) {
      special_tabs_to_close.push_back(i);
    }
  }

  for (std::size_t gi = state.editor_groups.size(); gi-- > 0;) {
    if (gi == state.focused_group_index || gi >= state.editor_groups.size()) {
      continue;
    }
    std::vector<std::size_t> group_tabs_to_close;
    auto& group_tabs = state.editor_groups[gi].open_tabs;
    for (std::size_t i = 0; i < group_tabs.size(); ++i) {
      TabEntry& tab = group_tabs[i];
      if (tab.kind == TabEntry::Kind::Editor && !tab.editor_state.has_value() &&
          tab.deferred_handle.has_value()) {
        auto& handle = *tab.deferred_handle;
        if (!handle.path.empty() && util::PathEqualsOrWithinNormalized(handle.path, old_path)) {
          handle.path = util::ReplacePathPrefix(handle.path, old_path, new_path).lexically_normal();
          tab.path = handle.path;
          tab.title = TabTitleForPath(tab.path, tab.untitled_number);
        }
        continue;
      }
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        if (RetargetSpecialTabForRename(tab, old_path, new_path, preserve_unsaved_state) ==
            SpecialTabRetargetOutcome::MustClose) {
          group_tabs_to_close.push_back(i);
        }
        continue;
      }
      auto& editor_state = *tab.editor_state;
      if (editor_state.content_pending()) {
        if (!editor_state.restore.path.empty() &&
            util::PathEqualsOrWithinNormalized(editor_state.restore.path, old_path)) {
          editor_state.restore.path =
              util::ReplacePathPrefix(editor_state.restore.path, old_path, new_path)
                  .lexically_normal();
          tab.path = editor_state.restore.path;
          tab.title = TabTitleForPath(tab.path, tab.untitled_number);
        }
        continue;
      }
      const std::filesystem::path current_path =
          tab.path.empty() ? operations_.editor_view_path(editor_state) : tab.path.lexically_normal();
      if (current_path.empty() || !PathEqualsOrWithin(current_path, old_path)) {
        continue;
      }
      const std::filesystem::path updated_path =
          util::ReplacePathPrefix(current_path, old_path, new_path).lexically_normal();
      if (!preserve_unsaved_state && editor_state.viewport.dirty()) {
        const std::size_t cursor_line = editor_state.viewport.cursor_line();
        const std::size_t cursor_column = editor_state.viewport.cursor_column();
        const std::size_t scroll_line = editor_state.viewport.scroll_line();
        const std::size_t horizontal_scroll = editor_state.viewport.horizontal_scroll();
        if (const editor::TextViewport* reopened_view = reopen_discarded_view(updated_path);
            reopened_view != nullptr) {
          editor_state.viewport = *reopened_view;
          editor_state.viewport.MoveCursorTo(cursor_line, cursor_column);
          editor_state.viewport.SetScrollLine(scroll_line);
          editor_state.viewport.SetHorizontalScroll(horizontal_scroll);
        } else {
          editor_state.viewport.SetPath(updated_path);
        }
      } else {
        editor_state.viewport.SetPath(updated_path);
      }
      editor_state.restore.path = updated_path;
      editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
      editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
      editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
      editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
      tab.path = updated_path;
      tab.title = updated_path.filename().string();
    }
    std::sort(group_tabs_to_close.rbegin(), group_tabs_to_close.rend());
    for (std::size_t index : group_tabs_to_close) {
      editor_tabs_.CloseGroupTab(gi, index);
    }
  }

  std::sort(special_tabs_to_close.rbegin(), special_tabs_to_close.rend());
  for (std::size_t index : special_tabs_to_close) {
    editor_tabs_.Close(index);
  }

  if (!state.overlay.workflow.compare_picker.path.empty() &&
      PathEqualsOrWithin(state.overlay.workflow.compare_picker.path, old_path)) {
    state.overlay.workflow.compare_picker.path =
        util::ReplacePathPrefix(state.overlay.workflow.compare_picker.path, old_path, new_path);
    if (state.overlay.visible && state.overlay.mode == OverlayMode::CommitPicker) {
      HideOverlay(state);
    }
  }
}

void PathMutationCoordinator::CloseOpenTabsForPath(const std::filesystem::path& path) {
  auto& state = CurrentProjectState();
  if (editor_tabs_.ActiveTabIsEditor()) {
    editor_tabs_.SyncActiveEditorTab();
  }

  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    auto& tab = state.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor) {
      continue;
    }
    std::filesystem::path current_path;
    if (tab.editor_state.has_value()) {
      current_path = operations_.editor_view_path(*tab.editor_state);
    } else if (tab.deferred_handle.has_value()) {
      current_path = util::NormalizedPath(tab.deferred_handle->path);
    } else {
      continue;
    }
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

  for (std::size_t gi = state.editor_groups.size(); gi-- > 0;) {
    if (gi == state.focused_group_index || gi >= state.editor_groups.size()) {
      continue;
    }
    std::vector<std::size_t> group_indices;
    const auto& tabs = state.editor_groups[gi].open_tabs;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      const TabEntry& tab = tabs[i];
      if (tab.kind != TabEntry::Kind::Editor) {
        if (CompareTabAffectedByPath(tab, normalized_path) ||
            MergeTabAffectedByPath(tab, normalized_path)) {
          group_indices.push_back(i);
        }
        continue;
      }
      std::filesystem::path current_path;
      if (tab.editor_state.has_value()) {
        current_path = operations_.editor_view_path(*tab.editor_state);
      } else if (tab.deferred_handle.has_value()) {
        current_path = util::NormalizedPath(tab.deferred_handle->path);
      } else {
        continue;
      }
      if (!current_path.empty() && PathEqualsOrWithin(current_path, normalized_path)) {
        group_indices.push_back(i);
      }
    }
    std::sort(group_indices.rbegin(), group_indices.rend());
    for (std::size_t index : group_indices) {
      editor_tabs_.CloseGroupTab(gi, index);
    }
  }

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
      HideOverlay(state);
    }
  }
}

}
