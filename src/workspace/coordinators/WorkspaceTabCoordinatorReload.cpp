#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>

#include "util/StringUtil.h"

namespace microide::workspace {

// Reloading open views from disk: the watcher's clean reload, the explicit
// discard-and-reload, and the identical-content rule that keeps a buffer (and its
// undo/redo history) when the file holds what it holds. Split out of
// WorkspaceTabCoordinator.cpp to keep that TU under the architecture lint's
// 900-line coordinator budget; one coherent family with one entry point.

bool TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path,
                                                  EchoGuard echo_guard) {
  return ReloadEditorTabsForPath(path, /*clean_only=*/true, echo_guard);
}

void TabCoordinator::ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path) {
  (void)ReloadEditorTabsForPath(path, /*clean_only=*/false);
}

bool TabCoordinator::ReloadEditorTabsForPath(const std::filesystem::path& path, bool clean_only,
                                             EchoGuard echo_guard) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  operations_.invalidate_editor_blame_path(normalized_path);

  // Reload every editor view on this path across ALL groups — a split view of the same
  // file in the non-focused group must not be left showing stale content (nor keep a
  // stale disk_signature that would later misfire self-write echo suppression).
  // Compare, do not materialize: this runs over every tab in every group, twice
  // (the any-match probe and the apply loop), and the dominant caller is opening a
  // file that is already open. Building a normalized `path` value per tab per scan
  // made that quadratic in tab count at ~12 allocations a step
  // (TD-2026-08-06-159).
  const auto matches = [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
           EditorViewPathIs(*tab.editor_state, normalized_path);
  };
  // A compare tab's editable side is a view of the same file (it shares the
  // editor tabs' document); it takes the reloaded document too, or the panes
  // diverge into one buffer per tab again.
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

  // Nothing changed underneath us: every open view of this path already records
  // the signature the file has right now, so a reload would read the whole file
  // off disk only to produce byte-identical content -- and then throw away every
  // derived cache in the process (widths, highlights, folds, undo history).
  //
  // The dominant caller is opening a file that is already open, which is what
  // Ctrl+P to a file you are already looking at does. VSCode just focuses the
  // tab there; before this, a 50k-line file paid a full file read plus a
  // whole-document width rebuild on every such open (TD-2026-08-06-138). The
  // focus-regain sweep (ReloadCleanOpenBuffersFromDisk) paid it once per open
  // buffer.
  //
  // `clean_only` only: the from-disk form is the banner's explicit "discard my
  // edits and reload", and a dirty buffer's recorded signature can legitimately
  // still match the file it was loaded from -- skipping there would silently
  // refuse to discard. mtime+size is the same equality the self-write echo
  // suppression in WorkspaceShellProjectChanges already trusts.
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

  // Capture just the delta the LSP re-sync needs (pre-reload line count + the first
  // line whose content differs from the reloaded buffer) from the first view we
  // replace. Both the old view and `reopened_view` (the after-content) exist here,
  // so this compares them line-by-line without materializing either whole buffer —
  // the full didChange later streams straight from the reloaded viewport. All
  // matching views share this path's single server document, so one sync suffices.
  std::size_t lsp_before_line_count = 0;
  std::size_t lsp_first_changed_line = 0;
  bool captured_lsp_before = false;
  bool reloaded_any = false;

  const std::size_t focused_index = state_.clamped_focused_group_index();
  for (std::size_t g = 0; g < state_.editor_groups.size(); ++g) {
    EditorGroup& group = state_.editor_groups[g];
    const bool is_focused_group = g == focused_index;
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      TabEntry& tab = group.open_tabs[i];
      if (matches_compare(tab) && !(clean_only && TabStateIsDirty(tab))) {
        // Same rule as the editor views below: the right side usually SHARES their
        // document, and repointing it alone would split one file into two buffers.
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
      // The file holds exactly what this buffer holds -- typically the echo of the
      // editor's own save, reported after it (a watcher verdict computed before the
      // save landed, a rescan sweep while a deferred save's signature is still in
      // flight). Swapping in the freshly read document would throw away the undo
      // AND redo history for nothing, which is what the editor-invariants sweep
      // caught; VS Code keeps the buffer. Take the new on-disk identity and move on.
      if (editor_state.content == EditorTabState::Content::Ready &&
          editor_state.viewport.HoldsSameFileContentAs(reopened_view)) {
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
      const editor::TextViewport* current_view = &editor_state.viewport;
      editor::TextViewport restored_view = reopened_view;
      restored_view.SetViewportSize(current_view->visible_lines(), current_view->visible_columns());
      restored_view.ApplyRestoredViewState(current_view->cursor_line(), current_view->cursor_column(),
                                           current_view->scroll_line(),
                                           current_view->horizontal_scroll());
      // Moved, not copied: `restored_view` is dead after this and its layout
      // cache now survives a move, so copying would deep-copy a per-line width
      // table (one machine word per line of the document) to throw it away one
      // statement later. Read the restored fields back off the tab's own
      // viewport, which is where they live now.
      editor_state.viewport = std::move(restored_view);
      editor_state.restore.path = normalized_path;
      editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
      editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
      editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
      editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
      editor_state.content = EditorTabState::Content::Ready;
      editor_state.folding_model->Clear();
      if (is_focused_group && i == group.active_tab_index) {
        SyncActiveEditorTabMetadata();
        operations_.request_editor_surface_redraw();
      }
    }
  }

  // Re-sync the LSP server's document mirror to the reloaded content (full
  // didChange). Fired once after all views are swapped, since the server tracks a
  // single document per path regardless of how many split views show it.
  if (captured_lsp_before && operations_.notify_lsp_buffer_reloaded) {
    operations_.notify_lsp_buffer_reloaded(reopened_view, lsp_before_line_count,
                                           lsp_first_changed_line);
  }
  return reloaded_any;
}

}  // namespace microide::workspace
