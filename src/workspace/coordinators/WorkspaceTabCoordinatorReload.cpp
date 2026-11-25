#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>

#include "util/PathMatch.h"
#include "util/StringUtil.h"

namespace microide::workspace {

bool TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path,
                                                  EchoGuard echo_guard,
                                                  std::vector<std::filesystem::path>* reloaded_paths) {
  bool reloaded = ReloadEditorTabsForPath(path, true, echo_guard);
  std::filesystem::path scratch;
  const std::filesystem::path& normal = util::NormalizedPathView(path, scratch);
  if (reloaded && reloaded_paths != nullptr) {
    reloaded_paths->push_back(normal);
  }
  for (const std::filesystem::path& alias : OpenSymlinkAliasesOf(normal)) {
    if (ReloadEditorTabsForPath(alias, true, EchoGuard::Check)) {
      reloaded = true;
      if (reloaded_paths != nullptr) {
        reloaded_paths->push_back(alias);
      }
    }
  }
  return reloaded;
}

void TabCoordinator::ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path) {
  (void)ReloadEditorTabsForPath(path, false);
}

bool TabCoordinator::RevertEditorTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  editor::TextViewport on_disk;
  if (!on_disk.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(on_disk);
  for (EditorGroup& group : state_.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->content != EditorTabState::Content::Ready ||
          !util::SameAsNormalizedPath(tab.editor_state->viewport.path(), normalized_path)) {
        continue;
      }
      if (tab.editor_state->viewport.RevertToDiskContent(on_disk)) {
        operations_.invalidate_editor_blame_path(normalized_path);
        return true;
      }
      ReloadEditorTabsForPathFromDisk(normalized_path);
      return true;
    }
  }
  return false;
}

void TabCoordinator::NoteDiskChangedUnderEditsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (EditorGroup& group : state_.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          util::SameAsNormalizedPath(tab.editor_state->viewport.path(), normalized_path)) {
        tab.editor_state->viewport.NoteDiskChangedUnderEdits();
      } else if (tab.kind == TabEntry::Kind::Compare && LoadedBufferViewOf(tab) != nullptr &&
                 util::SameAsNormalizedPath(tab.compare->right_viewport.path(), normalized_path)) {
        tab.compare->right_viewport.NoteDiskChangedUnderEdits();
      }
    }
  }
}

bool TabCoordinator::NoteMissedDiskChangeUnderEdits(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  bool could_read_clean = false;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          tab.editor_state->content == EditorTabState::Content::Ready &&
          EditorViewPathIs(*tab.editor_state, normalized_path) &&
          tab.editor_state->viewport.DirtyWithACleanPosition()) {
        could_read_clean = true;
      }
      if (tab.kind == TabEntry::Kind::Compare && LoadedBufferViewOf(tab) != nullptr &&
          util::SameAsNormalizedPath(tab.compare->right_viewport.path(), normalized_path) &&
          tab.compare->right_viewport.DirtyWithACleanPosition()) {
        could_read_clean = true;
      }
    }
  }
  if (!could_read_clean || DiskSignatureMatchesOpenView(normalized_path)) {
    return false;
  }
  NoteDiskChangedUnderEditsForPath(normalized_path);
  return true;
}

bool TabCoordinator::ReloadEditorTabsForPath(const std::filesystem::path& path, bool clean_only,
                                             EchoGuard echo_guard) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  operations_.invalidate_editor_blame_path(normalized_path);

  const auto matches = [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
           EditorViewPathIs(*tab.editor_state, normalized_path);
  };
  const auto matches_compare = [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
           tab.compare->right_editable &&
           util::SameAsNormalizedPath(tab.compare->right_viewport.path(), normalized_path);
  };
  bool any_match = false;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if ((matches(tab) || matches_compare(tab)) && !(clean_only && TabStateIsDirty(tab))) {
        any_match = true;
        break;
      }
    }
    if (any_match) {
      break;
    }
  }
  if (!any_match) {
    return false;
  }

  if (clean_only && echo_guard == EchoGuard::Check &&
      DiskSignatureMatchesOpenView(normalized_path)) {
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(reopened_view);
  operations_.apply_detected_indent_on_open(reopened_view);

  std::size_t lsp_before_line_count = 0;
  std::size_t lsp_first_changed_line = 0;
  bool captured_lsp_before = false;
  bool reloaded_any = false;

  const std::size_t focused_index = state_.clamped_focused_group_index();
  std::vector<std::pair<std::size_t, std::size_t>> not_loaded;
  const auto install = [&](EditorTabState& editor_state, const editor::TextViewport& source,
                           std::size_t cursor_line, std::size_t cursor_column,
                           std::size_t scroll_line, std::size_t horizontal_scroll,
                           bool active_in_focused_group) {
    editor::TextViewport restored_view = source;
    restored_view.SetViewportSize(editor_state.viewport.visible_lines(),
                                  editor_state.viewport.visible_columns());
    restored_view.ApplyRestoredViewState(cursor_line, cursor_column, scroll_line, horizontal_scroll);
    editor_state.viewport = std::move(restored_view);
    editor_state.restore.path = normalized_path;
    editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
    editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
    editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
    editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
    editor_state.content = EditorTabState::Content::Ready;
    editor_state.folding_model->Clear();
    if (active_in_focused_group) {
      SyncActiveEditorTabMetadata();
      operations_.request_editor_surface_redraw();
    }
  };
  for (std::size_t g = 0; g < state_.editor_groups.size(); ++g) {
    EditorGroup& group = state_.editor_groups[g];
    const bool is_focused_group = g == focused_index;
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      TabEntry& tab = group.open_tabs[i];
      if (matches_compare(tab) && !(clean_only && TabStateIsDirty(tab))) {
        if (tab.compare->right_viewport.HoldsSameFileContentAs(reopened_view)) {
          tab.compare->right_viewport.AdoptDiskBaselineOf(reopened_view);
          continue;
        }
        RepointCompareRightSideToReloadedBuffer(*tab.compare, reopened_view);
        reloaded_any = true;
        continue;
      }
      if (!matches(tab) || (clean_only && TabStateIsDirty(tab))) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      if (editor_state.content_pending()) {
        not_loaded.emplace_back(g, i);
        continue;
      }
      if (editor_state.viewport.HoldsSameFileContentAs(reopened_view)) {
        editor_state.viewport.AdoptDiskBaselineOf(reopened_view);
        continue;
      }
      reloaded_any = true;
      if (!captured_lsp_before) {
        const editor::TextBuffer& old_lines = editor_state.viewport.lines();
        const editor::TextBuffer& new_lines = reopened_view.lines();
        lsp_before_line_count = old_lines.size();
        const std::size_t common = std::min(old_lines.size(), new_lines.size());
        while (lsp_first_changed_line < common &&
               old_lines.LineView(lsp_first_changed_line) ==
                   new_lines.LineView(lsp_first_changed_line)) {
          ++lsp_first_changed_line;
        }
        captured_lsp_before = true;
      }
      const editor::TextViewport& current_view = editor_state.viewport;
      install(editor_state, reopened_view, current_view.cursor_line(), current_view.cursor_column(),
              current_view.scroll_line(), current_view.horizontal_scroll(),
              is_focused_group && i == group.active_tab_index);
    }
  }

  if (!not_loaded.empty()) {
    const editor::TextViewport* live = LiveBufferViewOfPath(state_.editor_groups, normalized_path);
    const editor::TextViewport& source = live != nullptr ? *live : reopened_view;
    for (const auto& [g, i] : not_loaded) {
      EditorGroup& group = state_.editor_groups[g];
      EditorTabState& editor_state = *group.open_tabs[i].editor_state;
      const auto restore = editor_state.restore;
      install(editor_state, source, restore.cursor_line, restore.cursor_column, restore.scroll_line,
              restore.horizontal_scroll, g == focused_index && i == group.active_tab_index);
      reloaded_any = true;
    }
  }

  if (captured_lsp_before && operations_.notify_lsp_buffer_reloaded) {
    operations_.notify_lsp_buffer_reloaded(reopened_view, lsp_before_line_count,
                                           lsp_first_changed_line);
  }
  return reloaded_any;
}

}
