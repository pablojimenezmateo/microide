#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/TabReorder.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport, std::uint32_t untitled_number) {
  if (viewport.path().empty() && viewport.is_placeholder()) {
    return "Welcome";
  }
  return TabTitleForPath(viewport.path(), untitled_number);
}

}  // namespace

TabCoordinator::TabCoordinator(ProjectCatalogState& project_catalog,
                               ProjectWorkspaceState& current_project_state,
                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      operations_(std::move(operations)) {}


std::string TabCoordinator::ActiveTitle() const {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return EditorTabLabel(state_.focused_group().welcome_surface.viewport, 0);
  }
  return state_.focused_group().active_tab().title;
}

bool TabCoordinator::Save(std::size_t index, SaveMode mode) {
  return SaveGroupTab(state_.clamped_focused_group_index(), index, mode);
}

bool TabCoordinator::SaveGroupTab(std::size_t group_index, std::size_t index, SaveMode mode) {
  if (group_index >= state_.editor_groups.size()) {
    return false;
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index >= group.open_tabs.size()) {
    return false;
  }

  const auto refresh_directory_tree = [this]() {
    state_.directory_tree.Refresh();
    if (operations_.request_automatic_git_sidebar_refresh) {
      operations_.request_automatic_git_sidebar_refresh();
    }
  };

  if (group.open_tabs[index].kind == TabEntry::Kind::Compare &&
      group.open_tabs[index].compare.has_value()) {
    auto& compare_tab = group.open_tabs[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    if (compare_tab.right_viewport.SaveWouldOverwriteUnseenDisk()) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            compare_tab.right_viewport.path().lexically_normal());
      }
      return false;
    }
    // Same preparation as an editor tab's save: the compare right pane is a real
    // file and this is the same Ctrl+S. Without it, save participants and
    // format-on-save applied to a buffer depending on which surface it was open in.
    // Deferred like an editor tab's when the caller asks: the formatter runs off
    // the shell thread and ApplyDeferredSaveFormat finds this tab by its run id.
    if (operations_.prepare_editor_view_for_save) {
      const SavePreparation prepared = operations_.prepare_editor_view_for_save(
          compare_tab.right_viewport.path(), compare_tab.right_viewport, nullptr, mode);
      if (prepared.deferred()) {
        compare_tab.pending_format_save.Arm(prepared.deferred_run_id,
                                            compare_tab.right_viewport.content_revision());
        return true;
      }
      if (!prepared.ok()) {
        return false;
      }
    }
    if (!RefuseIfChangedOnDisk(compare_tab.right_viewport,
                               compare_tab.right_viewport.path().lexically_normal())) {
      return false;
    }
    if (!RefuseIfReadOnly(compare_tab.right_viewport.path().lexically_normal(),
                          /*banner_shows=*/false)) {
      return false;
    }
    if (!compare_tab.right_viewport.Save(state_.write_gate())) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(compare_tab.right_viewport.path(), {});
      }
      return false;
    }
    // As the editor's Save: the pane is clean now, which is when its inline blame
    // shows, and the blame read for the content before the save is stale.
    operations_.invalidate_editor_blame_path(compare_tab.right_viewport.path().lexically_normal());
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(compare_tab.right_viewport.path());
    return true;
  }

  if (group.open_tabs[index].kind == TabEntry::Kind::Merge &&
      group.open_tabs[index].merge.has_value()) {
    auto& merge_tab = group.open_tabs[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (merge_tab.result_viewport.SaveWouldOverwriteUnseenDisk()) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            merge_tab.result_viewport.path().lexically_normal());
      }
      return false;
    }
    if (operations_.prepare_editor_view_for_save) {
      const SavePreparation prepared = operations_.prepare_editor_view_for_save(
          merge_tab.result_viewport.path(), merge_tab.result_viewport, nullptr, mode);
      if (prepared.deferred()) {
        merge_tab.pending_format_save.Arm(prepared.deferred_run_id,
                                          merge_tab.result_viewport.content_revision());
        return true;
      }
      if (!prepared.ok()) {
        return false;
      }
    }
    if (!RefuseIfChangedOnDisk(merge_tab.result_viewport,
                               merge_tab.result_viewport.path().lexically_normal())) {
      return false;
    }
    if (!RefuseIfReadOnly(merge_tab.result_viewport.path().lexically_normal(),
                          /*banner_shows=*/false)) {
      return false;
    }
    if (!merge_tab.result_viewport.Save(state_.write_gate())) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(merge_tab.result_viewport.path(), {});
      }
      return false;
    }
    merge_tab.persisted_output_baseline =
        merge_tab.result_viewport.SerializeDocumentText(merge_tab.result_line_ending);
    // Our own atomic write+rename bumps the mtime; sync disk_result_tick to it and
    // clear any prior external-stale flag so the async file-change event does not
    // misread this save as an external modification. Without this, saving the merge
    // result would permanently lock out "Mark Resolved" for the tab.
    merge_tab.disk_result_tick = util::FileModificationTick(merge_tab.result_viewport.path());
    merge_tab.external_result_stale = false;
    // As the editor's Save: the result pane is clean now, which is when its inline
    // blame shows, and nothing else repaints it (QA merge-toolbar).
    operations_.invalidate_editor_blame_path(merge_tab.result_viewport.path().lexically_normal());
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(merge_tab.result_viewport.path());
    return true;
  }

  if (group.open_tabs[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = group.open_tabs[index].editor_state;
  if (!editor_state.has_value()) {
    return false;
  }

  if (editor_state->content_pending()) {
    // The buffer is not the file yet — it is the empty stand-in a tab shows while
    // its bytes are read. There is nothing of the user's to write, and writing it
    // would truncate the file that is still being read. Reported as done, not as
    // a failure, so Ctrl+S on a loading tab is a no-op rather than an error toast.
    return true;
  }
  editor::TextViewport* candidate = &editor_state->viewport;
  if (candidate->path().empty()) {
    // Untitled buffers cannot be saved through this path; refuse if dirty.
    return !candidate->dirty();
  }
  const std::filesystem::path normalized_path = candidate->path().lexically_normal();
  // A clean buffer has nothing to write -- unless it has never been on disk (a
  // path opened before it existed, or just named by Save As): then the save is
  // what creates the file, empty or not.
  if (!candidate->dirty() && candidate->HasDiskSignature()) {
    operations_.notify_plugin_buffer_save(normalized_path);
    return true;
  }
  // Refuse to overwrite a file that changed on disk since we loaded/last saved
  // it. Surface the external-change banner instead so the user can choose to
  // Reload, Overwrite, or Keep. Checked before formatters run so we don't
  // mutate the buffer for a save we're about to abort.
  if (candidate->SaveWouldOverwriteUnseenDisk()) {
    if (operations_.request_external_change_banner) {
      operations_.request_external_change_banner(normalized_path);
    }
    return false;
  }
  if (!RefuseIfReadOnly(normalized_path, /*banner_shows=*/true)) {
    return false;
  }
  if (operations_.prepare_editor_view_for_save) {
    const SavePreparation prepared =
        operations_.prepare_editor_view_for_save(candidate->path(), *candidate, nullptr, mode);
    if (!prepared.ok()) {
      return false;
    }
    if (prepared.deferred()) {
      // Nothing is written yet, and that is the point: the shell thread is free
      // while the formatter runs. Record the run so its completion can find this
      // tab, and the revision so it can tell whether its answer is still about
      // this buffer.
      editor_state->pending_format_save.Arm(prepared.deferred_run_id,
                                            candidate->content_revision());
      return true;
    }
    // The conflict check above ran BEFORE the participants and the formatter, and
    // a formatter can take seconds: a file written meanwhile must not be
    // overwritten by the save it raced. One stat.
    if (!RefuseIfChangedOnDisk(*candidate, normalized_path)) {
      return false;
    }
  }
  editor_state->pending_format_save.Disarm();
  if (!candidate->Save(state_.write_gate())) {
    if (operations_.notify_save_failed) {
      operations_.notify_save_failed(candidate->path(), {});
    }
    return false;
  }
  // The tab's dirty marker is gone. The deferred completion repaints the strip;
  // a save that wrote at once must too, or a save no keystroke made (the command
  // line, a menu path that damages nothing else) leaves "*name" on screen.
  operations_.request_tab_strip_redraw();
  operations_.invalidate_editor_blame_path(normalized_path);
  operations_.notify_plugin_buffer_save(normalized_path);
  refresh_directory_tree();
  return true;
}

bool TabCoordinator::SaveGroupTabAs(std::size_t group_index, std::size_t index,
                                    const std::filesystem::path& path, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (group_index >= state_.editor_groups.size() ||
      index >= state_.editor_groups[group_index].open_tabs.size()) {
    return fail("No tab to save");
  }
  TabEntry& tab = state_.editor_groups[group_index].open_tabs[index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return fail("Only an editor tab can be saved as a file");
  }
  if (path.empty() || path.filename().empty()) {
    return fail("save needs a file name");
  }
  editor::TextViewport& viewport = tab.editor_state->viewport;
  const std::filesystem::path normalized = path.lexically_normal();
  if (normalized == viewport.path().lexically_normal()) {
    return SaveGroupTab(group_index, index, SaveMode::Blocking) || fail("Save failed");
  }
  std::error_code exists_error;
  if (std::filesystem::exists(normalized, exists_error)) {
    return fail(normalized.string() + " already exists");
  }
  // The write goes to the buffer's path, so bind first -- and undo the binding if
  // the write fails (a directory that cannot be created, no permission): as in VS
  // Code, a failed Save As leaves the buffer what it was, not named after a file
  // that does not exist.
  const std::filesystem::path previous = viewport.path();
  const util::FileSignature previous_signature = viewport.disk_signature();
  const std::filesystem::path previous_tab_path = tab.path;
  std::string previous_title = tab.title;
  viewport.RebindToPath(normalized);
  tab.path = normalized;
  tab.title = normalized.filename().string();
  if (!SaveGroupTab(group_index, index, SaveMode::Blocking)) {
    viewport.RebindToPath(previous, previous_signature);
    tab.path = previous_tab_path;
    tab.title = std::move(previous_title);
    return fail("Save failed");
  }
  if (!previous.empty()) {
    operations_.invalidate_editor_blame_path(previous.lexically_normal());
  }
  operations_.notify_plugin_buffer_open(normalized);
  // The tree follows the active file; a buffer saved under a new name is a new
  // file the tree can show, so select and reveal it as activating a tab does
  // (VS Code reveals the saved file). It used to leave the previous selection,
  // so F2 after a first save renamed whatever row was selected before.
  // The index has not seen the new file yet; Save As is rare and the rescan
  // is what makes the row exist to select.
  if (!state_.directory_tree.SelectPath(normalized)) {
    state_.directory_tree.Refresh();
  }
  if (state_.directory_tree.SelectPath(normalized) && operations_.reveal_selected_tree_sidebar_line) {
    operations_.reveal_selected_tree_sidebar_line();
  }
  // The buffer has a new name: its tab title, its breadcrumb, and -- through the
  // extension -- possibly its language, so its highlighting. A plain save repaints
  // only the dirty marker.
  operations_.request_active_tab_redraw(/*include_tree_sidebar=*/true);
  return true;
}

bool TabCoordinator::ActiveTabIsEditor() const {
  const EditorGroup& group = state_.focused_group();
  return group.has_active_tab() && group.active_tab().kind == TabEntry::Kind::Editor &&
         group.active_tab().editor_state.has_value();
}

TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() {
  return const_cast<TabEntry::EditorTabState*>(std::as_const(*this).ActiveEditorTab());
}

const TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  const auto& editor_state = state_.focused_group().active_tab().editor_state;
  return editor_state.has_value() ? &*editor_state : nullptr;
}

editor::TextViewport* TabCoordinator::ActiveEditorViewport() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return &state_.focused_group().welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

const editor::TextViewport* TabCoordinator::ActiveEditorViewport() const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return &state_.focused_group().welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

void TabCoordinator::Activate(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::Activate");
  perf_label.Field("index", static_cast<long long>(index));
  if (!state_.focused_group().open_tabs[index].path.empty()) {
    perf_label.Field("path", state_.focused_group().open_tabs[index].path);
  }
  util::StartupTrace::Scope trace_scope("TabCoordinator::Activate");
  util::PerformanceTrace::Scope perf_scope(perf_label.View());

  if (state_.focused_group().active_tab_index == index) {
    auto& active_tab = state_.focused_group().open_tabs[index];
    // Re-activating the already-active tab normally needs no load, but a tab whose
    // content is not here yet must still be hydrated or its pane renders empty.
    // BOTH un-hydrated forms qualify: no `editor_state` with a deferred handle,
    // and an `editor_state` whose `content` is not Ready — a split clone of a tab
    // that was still loading is the second, and checking only the first left it
    // empty and read-only for the life of the pane. Guarded on the un-hydrated
    // state so the common re-click stays a no-op and never re-snaps scroll onto
    // the caret.
    if (active_tab.kind == TabEntry::Kind::Editor &&
        (active_tab.editor_state.has_value() ? active_tab.editor_state->content_pending()
                                             : active_tab.deferred_handle.has_value())) {
      (void)LoadEditorTabForActivation(active_tab);
    }
    SyncActiveEditorTabMetadata();
    state_.surface.focus = FocusTarget::Editor;
    operations_.reset_caret_blink();
    const editor::TextViewport* active_vp = ActiveEditorViewport();
    operations_.request_active_tab_redraw(active_tab.kind == TabEntry::Kind::Editor &&
                                          active_vp != nullptr && !active_vp->path().empty());
    return;
  }

  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() && state_.focused_group().active_tab_index != index) {
    SyncActiveEditorTab();
  }

  state_.focused_group().active_tab_index = index;
  auto& tab = state_.focused_group().open_tabs[index];
  // Editor loading is best-effort: if the file disappeared while the IDE was
  // closed, we still want the tab strip + project tree to reflect this tab as
  // the active one (otherwise the activation looks like a no-op to the user).
  // LoadEditorTabForActivation returns false on failure but the post-activation
  // sync below still runs.
  (void)LoadEditorTabForActivation(tab);
  SyncActiveEditorTabMetadata();
  const editor::TextViewport* active_vp =
      (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value())
          ? &tab.editor_state->viewport
          : nullptr;
  const std::filesystem::path active_vp_path =
      active_vp != nullptr ? active_vp->path().lexically_normal() : std::filesystem::path{};
  if (tab.kind == TabEntry::Kind::Compare) {
    operations_.reveal_active_compare_selection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    operations_.reveal_active_merge_selection();
  } else if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty()) {
    util::StartupTrace::Scope select_path_scope("TabCoordinator::Activate::SelectDirectoryPath");
    if (state_.directory_tree.SelectPathIfVisible(active_vp_path)) {
      operations_.reveal_selected_tree_sidebar_line();
    }
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  // Engage the language server for the now-active editor document. This covers the
  // session-restore case (a file already open at startup, activated without ever
  // going through OpenFileInNewTab) and plain tab switches — without it, a
  // restored file's LSP stayed at "Starting..." with no diagnostics/semantic
  // colors until an edit or go-to-definition. SCHEDULED, not run inline: the
  // hydration (didOpen + semantic tokens + inlay hints, which serializes the whole
  // buffer) runs after the tab-switch frame is presented, so switching to a large
  // file never blocks the tab becoming visible. Idempotent, so switches are cheap.
  if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty() &&
      operations_.schedule_lsp_buffer_open) {
    operations_.schedule_lsp_buffer_open(active_vp_path);
  }
  operations_.request_active_tab_redraw(tab.kind == TabEntry::Kind::Editor &&
                                        !active_vp_path.empty());
}

void TabCoordinator::SyncActiveEditorTab() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  auto& editor_state = *tab.editor_state;
  if (editor_state.content_pending()) {
    tab.path = operations_.editor_view_path(editor_state);
    tab.title = TabTitleForPath(tab.path, tab.untitled_number);
    return;
  }
  editor_state.restore.path = editor_state.viewport.path().lexically_normal();
  editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
  editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
  editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
  editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
      &tab == &state_.focused_group().active_tab()) {
    SyncActiveEditorTabMetadata();
  }
}

bool TabCoordinator::ActivateCurrentTabAfterStateLoad() {
  // Eagerly hydrate the active editor tab of every non-focused group so a restored
  // split shows content in both panes, not just the focused one. The focused group
  // is hydrated below via Activate().
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    if (gi == state_.focused_group_index) {
      continue;
    }
    EditorGroup& group = state_.editor_groups[gi];
    if (group.active_tab_index >= group.open_tabs.size()) {
      continue;
    }
    TabEntry& tab = group.active_tab();
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      // Skip the preference re-apply for a freshly-hydrated tab: RestoreEditorTab
      // already applied current preferences, and re-running them here would fire
      // EnsureCursorVisible and clobber the restored scroll.
      const bool was_deferred = tab.editor_state->content_pending();
      if (EnsureEditorTabLoaded(tab) && !was_deferred) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (state_.focused_group().open_tabs.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(state_.focused_group().active_tab_index, state_.focused_group().open_tabs.size() - 1);
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size();
  Activate(active_index);
  return state_.focused_group().active_tab_index == active_index;
}

void TabCoordinator::SyncActiveEditorTabMetadata() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().active_tab();
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  std::filesystem::path active_path;
  if (tab.editor_state.has_value() && tab.editor_state->content_pending()) {
    // Not loaded (yet, or ever: a file deleted between restore and activation):
    // its identity is the restore target, not the empty stand-in's.
    active_path = operations_.editor_view_path(*tab.editor_state);
    if (!active_path.empty()) {
      tab.path = active_path;
      tab.title = active_path.filename().string();
    }
  } else if (tab.editor_state.has_value()) {
    const editor::TextViewport& viewport = tab.editor_state->viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport, tab.untitled_number);
  } else if (tab.deferred_handle.has_value() || !tab.path.empty()) {
    // No live viewport, but the tab has a real identity: a deferred tab whose
    // file failed to open (e.g. deleted out from under the IDE between restore
    // and first activation). Do NOT fall through to the group's welcome surface
    // -- its empty path would clobber the tab's filename/title, stranding the
    // tab as "Welcome"/"" in the strip and breaking the OpenFileInNewTab dedup
    // (keyed on tab.path). Keep the restored identity; recover it from the
    // deferred handle if the path was somehow lost.
    if (tab.path.empty() && tab.deferred_handle.has_value()) {
      tab.path = tab.deferred_handle->path.lexically_normal();
      tab.title = TabTitleForPath(tab.path, tab.untitled_number);
    }
    active_path = tab.path.lexically_normal();
  } else {
    // Truly empty editor tab (no state, no handle, no path): mirror the group
    // welcome surface, as before.
    const editor::TextViewport& viewport = state_.focused_group().welcome_surface.viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport, tab.untitled_number);
  }
  if (!active_path.empty() && state_.directory_tree.SelectPathIfVisible(active_path)) {
    operations_.reveal_selected_tree_sidebar_line();
  } else if (!active_path.empty() &&
             !state_.directory_tree.HasManuallyCollapsedAncestor(active_path) &&
             state_.directory_tree.SelectPath(active_path)) {
      operations_.reveal_selected_tree_sidebar_line();
  }
}

bool TabCoordinator::OpenUntitled() {
  if (state_.root.empty()) {
    return false;
  }
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Refuse a `tab`-command flood; see kMaxOpenTabsPerGroup.
  }

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  operations_.apply_editor_preferences(untitled_view);

  const std::uint32_t untitled_number = NextUntitledNumber(state_.editor_groups);
  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = UntitledTitle(untitled_number),
      .untitled_number = untitled_number,
      .editor_state = operations_.make_editor_tab_state(untitled_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  return true;
}
const editor::TextViewport* TabCoordinator::FindOpenEditorViewOfPath(
    const std::filesystem::path& normalized_path) const {
  return LiveBufferViewOfPath(state_.editor_groups, normalized_path);
}

bool TabCoordinator::OpenEditorViewForPath(const std::filesystem::path& path,
                                           editor::TextViewport& view) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const editor::TextViewport* live = FindOpenEditorViewOfPath(normalized_path);
      live != nullptr) {
    view = *live;  // shares the DocumentState; caret/scroll are the view's own
    return true;
  }
  if (!view.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(view);
  operations_.apply_detected_indent_on_open(view);
  return true;
}

bool TabCoordinator::ShouldOpenOffThread(const std::filesystem::path& normalized_path) const {
  if (!operations_.begin_async_file_read) {
    return false;
  }
  // A file already open somewhere shares that buffer instead of being read, so
  // its size is irrelevant.
  if (FindOpenEditorViewOfPath(normalized_path) != nullptr) {
    return false;
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(normalized_path, error);
  return !error && size >= kAsyncOpenThresholdBytes;
}

std::uint64_t TabCoordinator::BeginOffThreadOpen(const std::filesystem::path& normalized_path,
                                                 editor::TextViewport& view) const {
  const std::uint64_t read_id = operations_.begin_async_file_read(normalized_path);
  if (read_id == 0) {
    return 0;
  }
  // An empty buffer bound to the real path, so the tab strip, the breadcrumb and
  // the status bar all name the file that is arriving. Read-only until it does:
  // a keystroke landing here would otherwise become a buffer that shadows the
  // file and can be saved over it.
  view.LoadContent("", normalized_path);
  view.SetReadOnly(true);
  operations_.apply_editor_preferences(view);
  return read_id;
}

bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::OpenFileInNewTab");
  perf_label.Field("path", path);
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  if (state_.root.empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });

  {
    util::PerformanceTrace::Scope select_path_scope(
        "TabCoordinator::OpenFileInNewTab::SelectDirectoryPath");
    if (!state_.directory_tree.SelectPathIfVisible(normalized_path)) {
      state_.directory_tree.SelectPath(normalized_path);
    }
  }

  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t existing_index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(existing_index)) {
      ReloadCleanEditorTabsForPath(normalized_path);
    }
    Activate(existing_index);
    return true;
  }

  // Enforce the per-group ceiling before touching disk: an already-open file
  // reuses its tab via the dedup above, so a brand-new open past the cap is
  // refused here — before the file read + indent detection below — so a flood of
  // distinct paths cannot pay that I/O only to be rejected.
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }

  editor::TextViewport opened_view;
  std::uint64_t read_id = 0;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::OpenFileInNewTab::OpenFile");
    if (ShouldOpenOffThread(normalized_path)) {
      read_id = BeginOffThreadOpen(normalized_path, opened_view);
    }
    if (read_id == 0 && !OpenEditorViewForPath(normalized_path, opened_view)) {
      return false;
    }
  }

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  if (read_id != 0) {
    TabEntry::EditorTabState& editor_state =
        *state_.focused_group().open_tabs.back().editor_state;
    editor_state.content = EditorTabState::Content::Loading;
    editor_state.restore.path = normalized_path;
    editor_state.pending_load.Arm(read_id, editor_state.viewport.content_revision());
  }
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  // The plugin-visible open is the moment the buffer exists. For an off-thread
  // read that is the completion, not now, or a plugin reads an empty document
  // and caches it as the file.
  if (read_id == 0) {
    operations_.notify_plugin_buffer_open(normalized_path);
  }
  operations_.request_active_tab_redraw(true);
  return true;
}
bool TabCoordinator::ReopenEditor(DeferredTabHandle view) {
  if (state_.root.empty() || view.path.empty()) {
    return false;
  }
  view.path = view.path.lexically_normal();
  EditorGroup& group = state_.focused_group();
  const auto existing = std::find_if(group.open_tabs.begin(), group.open_tabs.end(),
                                     [&](const TabEntry& tab) {
                                       return tab.kind == TabEntry::Kind::Editor &&
                                              tab.path == view.path;
                                     });
  if (existing != group.open_tabs.end()) {
    Activate(static_cast<std::size_t>(std::distance(group.open_tabs.begin(), existing)));
    return true;
  }
  if (group.open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  const std::filesystem::path path = view.path;
  group.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path,
      .title = TabTitleForPath(path, 0),
      .editor_state = std::nullopt,
      .deferred_handle = std::move(view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  // A read that fails here keeps the tab naming its file, as a restored
  // session's does, so the next activation tries again.
  Activate(group.open_tabs.size() - 1);
  return true;
}

bool TabCoordinator::OpenNewBufferInNewTab(const std::filesystem::path& path) {
  if (state_.root.empty() || path.empty() || path.filename().empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code exists_error;
  if (std::filesystem::exists(normalized_path, exists_error)) {
    return OpenFileInNewTab(normalized_path);
  }
  EditorGroup& group = state_.focused_group();
  auto existing = std::find_if(group.open_tabs.begin(), group.open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });
  if (existing != group.open_tabs.end()) {
    Activate(static_cast<std::size_t>(std::distance(group.open_tabs.begin(), existing)));
    return true;
  }
  if (group.open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  editor::TextViewport opened_view;
  if (const editor::TextViewport* live = FindOpenEditorViewOfPath(normalized_path);
      live != nullptr) {
    opened_view = *live;  // the not-yet-saved buffer another group already holds
  } else {
    opened_view.LoadContent("", normalized_path);
    operations_.apply_editor_preferences(opened_view);
  }
  group.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  group.active_tab_index = group.open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.notify_plugin_buffer_open(normalized_path);
  operations_.request_active_tab_redraw(true);
  return true;
}

bool TabCoordinator::OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                                 std::string_view content,
                                                 std::string_view title) {
  if (state_.root.empty() || virtual_path.empty()) {
    return false;
  }

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == virtual_path;
                               });
  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(index) && existing->editor_state.has_value() &&
        operations_.editor_view_path(*existing->editor_state) == virtual_path) {
      existing->editor_state->viewport.ReloadPreservingViewState(content);
    }
    Activate(index);
    return true;
  }

  editor::TextViewport viewport;
  viewport.LoadContent(content, virtual_path);
  operations_.apply_editor_preferences(viewport);
  operations_.apply_detected_indent_on_open(viewport);

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // At the per-group ceiling; see kMaxOpenTabsPerGroup.
  }
  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = virtual_path,
      .title = std::string(title),
      .editor_state = operations_.make_editor_tab_state(viewport),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  return true;
}
void TabCoordinator::ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                               std::string_view content) {
  if (virtual_path.empty()) {
    return;
  }

  // Walk EVERY editor group, not just the focused one: a virtual document open in
  // both split panes must refresh both copies, matching ReloadEditorTabsForPath.
  bool reloaded_any = false;
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    EditorGroup& group = state_.editor_groups[gi];
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      auto& tab = group.open_tabs[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          TabStateIsDirty(tab)) {
        continue;
      }
      if (operations_.editor_view_path(*tab.editor_state) != virtual_path) {
        continue;
      }
      tab.editor_state->viewport.ReloadPreservingViewState(content);
      reloaded_any = true;
      if (i == group.active_tab_index) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (reloaded_any) {
    operations_.request_active_tab_redraw(false);
  }
}
std::filesystem::path TabCoordinator::LspCloseCandidatePath(const TabEntry& tab) const {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
    return operations_.editor_view_path(*tab.editor_state);
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    const auto& compare_tab = *tab.compare;
    if (compare_tab.right_editable && !compare_tab.right_viewport.path().empty()) {
      return compare_tab.right_viewport.path().lexically_normal();
    }
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    const auto& merge_tab = *tab.merge;
    if (!merge_tab.result_viewport.path().empty()) {
      return merge_tab.result_viewport.path().lexically_normal();
    }
  }
  return {};
}

void TabCoordinator::CancelPendingAsyncRead(const TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      !operations_.cancel_async_file_read) {
    return;
  }
  const std::uint64_t read_id = tab.editor_state->pending_load.id();
  if (read_id != 0) {
    operations_.cancel_async_file_read(read_id);
  }
}

void TabCoordinator::MaybeNotifyLspClose(const TabEntry& tab) {
  const std::filesystem::path path = LspCloseCandidatePath(tab);
  if (!path.empty() && operations_.count_open_buffer_views(path) == 1) {
    operations_.notify_lsp_buffer_close(path);
  }
}

bool TabCoordinator::RefuseIfChangedOnDisk(const editor::TextViewport& viewport,
                                           const std::filesystem::path& normalized_path) {
  if (!viewport.SaveWouldOverwriteUnseenDisk()) {
    return true;
  }
  if (operations_.request_external_change_banner) {
    operations_.request_external_change_banner(normalized_path);
  }
  return false;
}

bool TabCoordinator::RefuseIfReadOnly(const std::filesystem::path& normalized_path,
                                      bool banner_shows) {
  // The atomic save would rename a temporary file over a read-only one -- which
  // needs only the directory's permission -- and leave a read-only file the
  // editor had just changed (TD-2026-10-22-380).
  if (!state_.write_gate().IsReadOnly(normalized_path)) {
    return true;
  }
  if (banner_shows) {
    SetEditorBanner(state_, EditorBannerState::Kind::ReadOnly, normalized_path);
    if (operations_.request_editor_surface_redraw) {
      operations_.request_editor_surface_redraw();
    }
  } else if (operations_.notify_save_failed) {
    operations_.notify_save_failed(normalized_path, "the file is read-only");
  }
  return false;
}

TabCoordinator::SaveForCloseResult TabCoordinator::SaveForClose(std::size_t index) {
  const std::size_t group_index = state_.clamped_focused_group_index();
  // Deferred, not Blocking: the whole point is that the shell thread is free
  // while the formatter runs. What the close path cannot do is close BEFORE the
  // write lands, so a deferred tab is handed back for a save continuation.
  if (!SaveGroupTab(group_index, index, SaveMode::Deferred)) {
    return {};
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index < group.open_tabs.size() && group.open_tabs[index].editor_state.has_value() &&
      group.open_tabs[index].editor_state->pending_format_save.armed()) {
    return {.saved = true, .deferred_tab_id = EnsureTabStableId(group.open_tabs[index])};
  }
  // Nothing was deferred — no formatter, or a clean buffer — so the file is
  // already written and the close is the same immediate one it always was.
  Close(index);
  return {.saved = true};
}

void TabCoordinator::RememberClosedEditor(const TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || tab.path.empty() || !tab.path.is_absolute()) {
    return;  // an untitled buffer has nothing to reopen
  }
  std::optional<DeferredTabHandle> view = EditorTabViewState(tab);
  DeferredTabHandle entry = view.has_value() ? std::move(*view) : DeferredTabHandle{};
  entry.path = tab.path.lexically_normal();
  auto& closed = state_.recently_closed_editors;
  std::erase_if(closed, [&](const auto& old) { return old.path == entry.path; });
  if (closed.size() >= ProjectWorkspaceState::kMaxRecentlyClosedEditors) {
    closed.erase(closed.begin());
  }
  closed.push_back(std::move(entry));
}

void TabCoordinator::ObserveNavigationLocation() {
  const EditorGroup& group = state_.focused_group();
  if (group.active_tab_index >= group.open_tabs.size()) {
    return;
  }
  const TabEntry& tab = group.active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      !tab.path.is_absolute()) {
    return;  // compare/merge/virtual/untitled tabs are not places to go back to
  }
  const EditorTabState& editor = *tab.editor_state;
  // A tab still loading shows where its caret will land, as Reopen Closed Editor does.
  state_.navigation_history.Observe(
      tab.path, editor.content_pending() ? editor.restore.cursor_line : editor.viewport.cursor_line(),
      editor.content_pending() ? editor.restore.cursor_column : editor.viewport.cursor_column());
}

std::optional<EditorNavigationHistory::Location> TabCoordinator::StepNavigation(int direction) {
  // The frame that would have observed the latest caret move may not have run yet.
  ObserveNavigationLocation();
  return state_.navigation_history.Step(direction, [](const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
  });
}

void TabCoordinator::Close(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  const bool closing_active = index == state_.focused_group().active_tab_index;
  const TabEntry& closing_tab = state_.focused_group().open_tabs[index];

  // No SyncActiveEditorTab() here: closing a non-active tab does not change the
  // active tab, and every persistence/deactivation path re-syncs the active tab
  // (SaveSessionState and StoreCurrentProjectState both call it), so capturing its
  // viewport now would be redundant work — costly in bulk closes via the tree
  // traversals in SyncActiveEditorTabMetadata.
  MaybeNotifyLspClose(closing_tab);
  CancelPendingAsyncRead(closing_tab);
  RememberClosedEditor(closing_tab);

  state_.focused_group().open_tabs.erase(state_.focused_group().open_tabs.begin() + static_cast<std::ptrdiff_t>(index));

  if (state_.focused_group().open_tabs.empty()) {
    // Closing the last tab of a split group collapses that group; the surviving
    // group takes the full editor area.
    if (state_.editor_groups.size() >= 2) {
      CollapseFocusedGroup();
      const editor::TextViewport* active_vp = ActiveEditorViewport();
      operations_.ensure_active_tab_visible();
      operations_.request_active_tab_redraw(active_vp != nullptr && !active_vp->path().empty());
      return;
    }
    state_.focused_group().active_tab_index = 0;
    state_.focused_group().tab_scroll_index = 0;
    state_.focused_group().welcome_surface.viewport.SetPlaceholderText("microide\n\n"
                                            "Project loaded.\n"
                                            "Use the sidebar to open files.\n");
    state_.surface.focus = FocusTarget::Editor;
    operations_.request_active_tab_redraw(false);
    return;
  }

  if (index < state_.focused_group().active_tab_index) {
    --state_.focused_group().active_tab_index;
  } else if (index == state_.focused_group().active_tab_index) {
    state_.focused_group().active_tab_index = std::min(index, state_.focused_group().open_tabs.size() - 1);
    auto& tab = state_.focused_group().active_tab();
    if (tab.kind == TabEntry::Kind::Editor) {
      // Promote the neighbor through the same loader Activate uses so a deferred
      // (session-restored, never-activated) tab restores its cursor/scroll/
      // selection instead of opening fresh at (0,0).
      (void)LoadEditorTabForActivation(tab);
    } else if (tab.kind == TabEntry::Kind::Compare) {
      // Promoting a compare/merge tab on close must scroll its active selection
      // into view, exactly as Activate() does — otherwise the revealed tab's
      // selected hunk can sit off-screen until the next interaction.
      operations_.reveal_active_compare_selection();
    } else if (tab.kind == TabEntry::Kind::Merge) {
      operations_.reveal_active_merge_selection();
    }
    if (!tab.path.empty()) {
      state_.directory_tree.SelectPath(tab.path);
      operations_.reveal_selected_tree_sidebar_line();
    }
    state_.surface.focus = FocusTarget::Editor;
  }

  state_.focused_group().tab_scroll_index = std::clamp(state_.focused_group().tab_scroll_index, 0,
                                       std::max(0, static_cast<int>(state_.focused_group().open_tabs.size()) - 1));
  operations_.ensure_active_tab_visible();
  if (closing_active) {
    const editor::TextViewport* active_vp = ActiveEditorViewport();
    operations_.request_active_tab_redraw(active_vp != nullptr && !active_vp->path().empty());
  } else {
    operations_.request_tab_strip_redraw();
  }
}
bool TabCoordinator::MoveActiveTo(std::size_t index) {
  if (!ReorderActive(state_.focused_group().open_tabs, state_.focused_group().active_tab_index, index)) {
    return false;
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_tab_strip_redraw();
  return true;
}

}  // namespace microide::workspace
