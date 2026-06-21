#include "workspace/WorkspaceTabCoordinator.h"

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
#include "workspace/TabReorder.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "Welcome" : "Untitled";
}

void RestoreViewportText(editor::TextViewport& viewport, std::string_view text) {
  const std::size_t visible_lines = viewport.visible_lines();
  const std::size_t visible_columns = viewport.visible_columns();
  const std::size_t cursor_line = viewport.cursor_line();
  const std::size_t cursor_column = viewport.cursor_column();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t horizontal_scroll = viewport.horizontal_scroll();
  const auto selection = viewport.selection_range();
  const std::filesystem::path path = viewport.path();
  const auto line_ending = viewport.line_ending();

  viewport.LoadContent(text, path, line_ending);
  viewport.SetViewportSize(visible_lines, visible_columns);
  viewport.MoveCursorTo(cursor_line, cursor_column);
  viewport.SetScrollLine(scroll_line);
  viewport.SetHorizontalScroll(horizontal_scroll);
  if (selection.has_value()) {
    viewport.MoveCursorTo(selection->start.line, selection->start.column);
    viewport.MoveCursorTo(selection->end.line, selection->end.column, true);
  }
}

}  // namespace

TabCoordinator::TabCoordinator(ProjectCatalogState& project_catalog,
                               ProjectWorkspaceState& current_project_state,
                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      operations_(std::move(operations)) {}

bool TabCoordinator::TabStateIsDirty(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    return tab.compare->right_editable && tab.compare->right_viewport.dirty();
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    return tab.merge->result_viewport.dirty();
  }
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }
  return tab.editor_state->viewport.dirty();
}

std::string TabCoordinator::ActiveTitle() const {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return EditorTabLabel(state_.focused_group().welcome_surface.viewport);
  }
  return state_.focused_group().open_tabs[state_.focused_group().active_tab_index].title;
}

bool TabCoordinator::Save(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return false;
  }

  const auto refresh_directory_tree = [this]() {
    state_.directory_tree.Refresh();
    if (operations_.request_automatic_git_sidebar_refresh) {
      operations_.request_automatic_git_sidebar_refresh();
    }
  };

  if (state_.focused_group().open_tabs[index].kind == TabEntry::Kind::Compare &&
      state_.focused_group().open_tabs[index].compare.has_value()) {
    auto& compare_tab = state_.focused_group().open_tabs[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    if (compare_tab.right_viewport.DetectDiskConflict() !=
        editor::TextViewport::DiskConflict::None) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            compare_tab.right_viewport.path().lexically_normal());
      }
      return false;
    }
    if (!compare_tab.right_viewport.Save()) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(compare_tab.right_viewport.path());
      }
      return false;
    }
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(compare_tab.right_viewport.path());
    return true;
  }

  if (state_.focused_group().open_tabs[index].kind == TabEntry::Kind::Merge &&
      state_.focused_group().open_tabs[index].merge.has_value()) {
    auto& merge_tab = state_.focused_group().open_tabs[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (merge_tab.result_viewport.DetectDiskConflict() !=
        editor::TextViewport::DiskConflict::None) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            merge_tab.result_viewport.path().lexically_normal());
      }
      return false;
    }
    if (!merge_tab.result_viewport.Save()) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(merge_tab.result_viewport.path());
      }
      return false;
    }
    merge_tab.persisted_output_baseline =
        util::SerializeLines(merge_tab.result_viewport.lines(), merge_tab.result_line_ending);
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(merge_tab.result_viewport.path());
    return true;
  }

  if (state_.focused_group().open_tabs[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = state_.focused_group().open_tabs[index].editor_state;
  if (!editor_state.has_value()) {
    return false;
  }

  editor::TextViewport* candidate = &editor_state->viewport;
  if (candidate->path().empty()) {
    // Untitled buffers cannot be saved through this path; refuse if dirty.
    return !candidate->dirty();
  }
  const std::filesystem::path normalized_path = candidate->path().lexically_normal();
  if (!candidate->dirty()) {
    operations_.notify_plugin_buffer_save(normalized_path);
    return true;
  }
  // Refuse to overwrite a file that changed on disk since we loaded/last saved
  // it. Surface the external-change banner instead so the user can choose to
  // Reload, Overwrite, or Keep. Checked before formatters run so we don't
  // mutate the buffer for a save we're about to abort.
  if (candidate->DetectDiskConflict() != editor::TextViewport::DiskConflict::None) {
    if (operations_.request_external_change_banner) {
      operations_.request_external_change_banner(normalized_path);
    }
    return false;
  }
  if (operations_.prepare_editor_view_for_save &&
      !operations_.prepare_editor_view_for_save(candidate->path(), *candidate, nullptr)) {
    return false;
  }
  if (!candidate->Save()) {
    if (operations_.notify_save_failed) {
      operations_.notify_save_failed(candidate->path());
    }
    return false;
  }
  operations_.invalidate_editor_blame_path(normalized_path);
  operations_.notify_plugin_buffer_save(normalized_path);
  refresh_directory_tree();
  return true;
}

bool TabCoordinator::IsDirty(std::size_t index) const {
  return index < state_.focused_group().open_tabs.size() && TabStateIsDirty(state_.focused_group().open_tabs[index]);
}

std::vector<std::size_t> TabCoordinator::DirtyIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state_.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    if (IsDirty(i)) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> TabCoordinator::DirtyIndicesForProject(std::size_t project_index) const {
  if (project_index >= project_catalog_.entries.size()) {
    return {};
  }
  if (!state_.root.empty() && project_index == project_catalog_.active_index) {
    return DirtyIndices();
  }
  const auto* project_state = project_catalog_.entries[project_index].get();
  if (project_state == nullptr) {
    return {};
  }
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(project_state->focused_group().open_tabs.size());
  for (std::size_t i = 0; i < project_state->focused_group().open_tabs.size(); ++i) {
    if (TabStateIsDirty(project_state->focused_group().open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

bool TabCoordinator::ActiveTabIsEditor() const {
  return state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
         state_.focused_group().open_tabs[state_.focused_group().active_tab_index].kind == TabEntry::Kind::Editor &&
         state_.focused_group().open_tabs[state_.focused_group().active_tab_index].editor_state.has_value();
}

TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.focused_group().open_tabs[state_.focused_group().active_tab_index].editor_state.value();
}

const TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.focused_group().open_tabs[state_.focused_group().active_tab_index].editor_state.value();
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
  std::string perf_label = "TabCoordinator::Activate";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(index=" + std::to_string(index);
    if (!state_.focused_group().open_tabs[index].path.empty()) {
      perf_label += ",path=" + state_.focused_group().open_tabs[index].path.string();
    }
    perf_label += ")";
  }
  util::StartupTrace::Scope trace_scope("TabCoordinator::Activate");
  util::PerformanceTrace::Scope perf_scope(perf_label);

  if (state_.focused_group().active_tab_index == index) {
    auto& active_tab = state_.focused_group().open_tabs[index];
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
  // The lambda returns false on failure but doesn't skip the post-activation
  // sync below.
  const auto attempt_editor_load = [&]() -> bool {
    if (tab.kind != TabEntry::Kind::Editor) {
      return true;
    }
    if (tab.editor_state.has_value()) {
      if (!EnsureEditorTabLoaded(tab)) {
        return false;
      }
      operations_.apply_editor_preferences(tab.editor_state->viewport);
      return true;
    }
    if (tab.deferred_handle.has_value()) {
      editor::TextViewport loaded_view;
      const std::filesystem::path deferred_path = tab.deferred_handle->path.lexically_normal();
      if (deferred_path.empty() || !loaded_view.OpenFile(deferred_path)) {
        return false;
      }
      operations_.apply_editor_preferences(loaded_view);
      operations_.apply_detected_indent_on_open(loaded_view);
      loaded_view.MoveCursorTo(tab.deferred_handle->cursor_line,
                               tab.deferred_handle->cursor_column);
      loaded_view.SetScrollLine(tab.deferred_handle->scroll_line);
      loaded_view.SetHorizontalScroll(tab.deferred_handle->horizontal_scroll);
      if (tab.deferred_handle->selection.has_value()) {
        const auto& selection = *tab.deferred_handle->selection;
        loaded_view.MoveCursorTo(selection.start.line, selection.start.column);
        loaded_view.MoveCursorTo(selection.end.line, selection.end.column, true);
      }
      tab.editor_state = operations_.make_editor_tab_state(loaded_view);
      tab.deferred_handle.reset();
      return true;
    }
    editor::TextViewport loaded_view;
    if (!loaded_view.OpenFile(tab.path)) {
      return false;
    }
    operations_.apply_editor_preferences(loaded_view);
    operations_.apply_detected_indent_on_open(loaded_view);
    tab.editor_state = operations_.make_editor_tab_state(loaded_view);
    return true;
  };
  (void)attempt_editor_load();
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
  operations_.request_active_tab_redraw(tab.kind == TabEntry::Kind::Editor &&
                                        !active_vp_path.empty());
}

void TabCoordinator::SyncActiveEditorTab() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  auto& editor_state = *tab.editor_state;
  if (editor_state.needs_restore) {
    tab.path = operations_.editor_view_path(editor_state);
    tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
    return;
  }
  editor_state.restored_path = editor_state.viewport.path().lexically_normal();
  editor_state.restored_cursor_line = editor_state.viewport.cursor_line();
  editor_state.restored_cursor_column = editor_state.viewport.cursor_column();
  editor_state.restored_scroll_line = editor_state.viewport.scroll_line();
  editor_state.restored_horizontal_scroll = editor_state.viewport.horizontal_scroll();
  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
      &tab == &state_.focused_group().open_tabs[state_.focused_group().active_tab_index]) {
    SyncActiveEditorTabMetadata();
  }
}

bool TabCoordinator::ActivateCurrentTabAfterStateLoad() {
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

  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const editor::TextViewport* viewport = tab.editor_state.has_value()
                                             ? &tab.editor_state->viewport
                                             : &state_.focused_group().welcome_surface.viewport;

  const std::filesystem::path active_path =
      viewport != nullptr ? viewport->path().lexically_normal() : std::filesystem::path{};
  tab.path = active_path;
  tab.title = viewport != nullptr ? EditorTabLabel(*viewport) : "untitled";
  if (!active_path.empty() && state_.directory_tree.SelectPathIfVisible(active_path)) {
    operations_.reveal_selected_tree_sidebar_line();
  } else if (!active_path.empty() &&
             !state_.directory_tree.HasManuallyCollapsedAncestor(active_path) &&
             state_.directory_tree.SelectPath(active_path)) {
      operations_.reveal_selected_tree_sidebar_line();
  }
}

void TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  operations_.invalidate_editor_blame_path(normalized_path);

  std::vector<std::size_t> matching_clean_tab_indices;
  matching_clean_tab_indices.reserve(state_.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() || IsDirty(i)) {
      continue;
    }
    if (operations_.editor_view_path(*tab.editor_state) == normalized_path) {
      matching_clean_tab_indices.push_back(i);
    }
  }
  if (matching_clean_tab_indices.empty()) {
    return;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(normalized_path)) {
    return;
  }
  operations_.apply_editor_preferences(reopened_view);
  operations_.apply_detected_indent_on_open(reopened_view);

  for (std::size_t i : matching_clean_tab_indices) {
    auto& editor_state = *state_.focused_group().open_tabs[i].editor_state;
    const editor::TextViewport* current_view = &editor_state.viewport;
    editor::TextViewport restored_view = reopened_view;
    restored_view.SetViewportSize(current_view->visible_lines(), current_view->visible_columns());
    restored_view.MoveCursorTo(current_view->cursor_line(), current_view->cursor_column());
    restored_view.SetScrollLine(current_view->scroll_line());
    restored_view.SetHorizontalScroll(current_view->horizontal_scroll());
    editor_state.viewport = restored_view;
    editor_state.restored_path = normalized_path;
    editor_state.restored_cursor_line = restored_view.cursor_line();
    editor_state.restored_cursor_column = restored_view.cursor_column();
    editor_state.restored_scroll_line = restored_view.scroll_line();
    editor_state.restored_horizontal_scroll = restored_view.horizontal_scroll();
    editor_state.needs_restore = false;
    editor_state.folding_model->Clear();
    if (i == state_.focused_group().active_tab_index) {
      SyncActiveEditorTabMetadata();
      operations_.request_editor_surface_redraw();
    }
  }
}

void TabCoordinator::ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  operations_.invalidate_editor_blame_path(normalized_path);

  std::vector<std::size_t> matching_tab_indices;
  matching_tab_indices.reserve(state_.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    const auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    if (operations_.editor_view_path(*tab.editor_state) == normalized_path) {
      matching_tab_indices.push_back(i);
    }
  }
  if (matching_tab_indices.empty()) {
    return;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(normalized_path)) {
    return;
  }
  operations_.apply_editor_preferences(reopened_view);
  operations_.apply_detected_indent_on_open(reopened_view);

  for (std::size_t i : matching_tab_indices) {
    auto& editor_state = *state_.focused_group().open_tabs[i].editor_state;
    const editor::TextViewport* current_view = &editor_state.viewport;
    editor::TextViewport restored_view = reopened_view;
    restored_view.SetViewportSize(current_view->visible_lines(), current_view->visible_columns());
    restored_view.MoveCursorTo(current_view->cursor_line(), current_view->cursor_column());
    restored_view.SetScrollLine(current_view->scroll_line());
    restored_view.SetHorizontalScroll(current_view->horizontal_scroll());
    editor_state.viewport = restored_view;
    editor_state.restored_path = normalized_path;
    editor_state.restored_cursor_line = restored_view.cursor_line();
    editor_state.restored_cursor_column = restored_view.cursor_column();
    editor_state.restored_scroll_line = restored_view.scroll_line();
    editor_state.restored_horizontal_scroll = restored_view.horizontal_scroll();
    editor_state.needs_restore = false;
    editor_state.folding_model->Clear();
    if (i == state_.focused_group().active_tab_index) {
      SyncActiveEditorTabMetadata();
      operations_.request_editor_surface_redraw();
    }
  }
}

bool TabCoordinator::OpenUntitled() {
  if (state_.root.empty()) {
    return false;
  }

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  operations_.apply_editor_preferences(untitled_view);

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
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
bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  std::string perf_label = "TabCoordinator::OpenFileInNewTab";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(path=" + path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
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

  editor::TextViewport opened_view;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::OpenFileInNewTab::OpenFile");
    if (!opened_view.OpenFile(normalized_path)) {
      return false;
    }
  }
  operations_.apply_editor_preferences(opened_view);
  operations_.apply_detected_indent_on_open(opened_view);

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
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
      RestoreViewportText(existing->editor_state->viewport, content);
    }
    Activate(index);
    return true;
  }

  editor::TextViewport viewport;
  viewport.LoadContent(content, virtual_path);
  operations_.apply_editor_preferences(viewport);
  operations_.apply_detected_indent_on_open(viewport);

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

  bool reloaded_any = false;
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() || IsDirty(i)) {
      continue;
    }

    if (operations_.editor_view_path(*tab.editor_state) != virtual_path) {
      continue;
    }
    RestoreViewportText(tab.editor_state->viewport, content);
    reloaded_any = true;
    if (i != state_.focused_group().active_tab_index) {
      continue;
    }
    operations_.apply_editor_preferences(tab.editor_state->viewport);
  }

  if (reloaded_any) {
    operations_.request_active_tab_redraw(false);
  }
}
void TabCoordinator::Close(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  const bool closing_active = index == state_.focused_group().active_tab_index;
  const TabEntry& closing_tab = state_.focused_group().open_tabs[index];

  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() && index != state_.focused_group().active_tab_index) {
    SyncActiveEditorTab();
  }

  if (closing_tab.kind == TabEntry::Kind::Editor && closing_tab.editor_state.has_value()) {
    const std::filesystem::path path = operations_.editor_view_path(*closing_tab.editor_state);
    if (!path.empty() && operations_.count_open_buffer_views(path) == 1) {
      operations_.notify_lsp_buffer_close(path);
    }
  } else if (closing_tab.kind == TabEntry::Kind::Compare && closing_tab.compare.has_value()) {
    const auto& compare_tab = *closing_tab.compare;
    if (compare_tab.right_editable && !compare_tab.right_viewport.path().empty() &&
        operations_.count_open_buffer_views(compare_tab.right_viewport.path()) == 1) {
      operations_.notify_lsp_buffer_close(compare_tab.right_viewport.path());
    }
  } else if (closing_tab.kind == TabEntry::Kind::Merge && closing_tab.merge.has_value()) {
    const auto& merge_tab = *closing_tab.merge;
    if (!merge_tab.result_viewport.path().empty() &&
        operations_.count_open_buffer_views(merge_tab.result_viewport.path()) == 1) {
      operations_.notify_lsp_buffer_close(merge_tab.result_viewport.path());
    }
  }

  state_.focused_group().open_tabs.erase(state_.focused_group().open_tabs.begin() + static_cast<std::ptrdiff_t>(index));

  if (state_.focused_group().open_tabs.empty()) {
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
    auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      if (EnsureEditorTabLoaded(tab)) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (loaded_view.OpenFile(tab.path)) {
        operations_.apply_editor_preferences(loaded_view);
        operations_.apply_detected_indent_on_open(loaded_view);
        tab.editor_state = operations_.make_editor_tab_state(loaded_view);
      }
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
