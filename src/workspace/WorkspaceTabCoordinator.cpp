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

#include "util/StringUtil.h"
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
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      tab.editor_state->views.empty()) {
    return false;
  }
  return std::any_of(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                     [](const auto& view) { return view.viewport.dirty(); });
}

std::string TabCoordinator::ActiveTitle() const {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return EditorTabLabel(state_.welcome_surface.viewport);
  }
  return state_.open_tabs[state_.active_tab_index].title;
}

bool TabCoordinator::Save(std::size_t index) {
  if (index >= state_.open_tabs.size()) {
    return false;
  }

  if (state_.open_tabs[index].kind == TabEntry::Kind::Compare &&
      state_.open_tabs[index].compare.has_value()) {
    auto& compare_tab = state_.open_tabs[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    if (!compare_tab.right_viewport.Save()) {
      return false;
    }
    state_.directory_tree.Refresh();
    operations_.notify_plugin_buffer_save(compare_tab.right_viewport.path());
    return true;
  }

  if (state_.open_tabs[index].kind == TabEntry::Kind::Merge &&
      state_.open_tabs[index].merge.has_value()) {
    auto& merge_tab = state_.open_tabs[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (!merge_tab.result_viewport.Save()) {
      return false;
    }
    merge_tab.persisted_output_baseline =
        util::SerializeLines(merge_tab.result_viewport.lines(), merge_tab.result_line_ending);
    state_.directory_tree.Refresh();
    operations_.notify_plugin_buffer_save(merge_tab.result_viewport.path());
    return true;
  }

  if (state_.open_tabs[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = state_.open_tabs[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  bool attempted_save = false;
  std::set<std::filesystem::path> open_paths;
  std::vector<std::filesystem::path> saved_paths;
  for (auto& view : editor_state->views) {
    editor::TextViewport* candidate = &view.viewport;
    if (candidate->path().empty()) {
      if (candidate->dirty()) {
        return false;
      }
      continue;
    }
    open_paths.insert(candidate->path().lexically_normal());
    if (!candidate->dirty()) {
      continue;
    }
    if (operations_.prepare_editor_view_for_save &&
        !operations_.prepare_editor_view_for_save(candidate->path(), *candidate, nullptr)) {
      return false;
    }
    if (!candidate->Save()) {
      return false;
    }
    attempted_save = true;
    saved_paths.push_back(candidate->path().lexically_normal());
  }
  if (attempted_save) {
    for (const auto& path : saved_paths) {
      operations_.invalidate_editor_blame_path(path);
      operations_.notify_plugin_buffer_save(path);
    }
    state_.directory_tree.Refresh();
    return true;
  }
  for (const auto& path : open_paths) {
    operations_.notify_plugin_buffer_save(path);
  }
  return !editor_state->views.empty();
}

bool TabCoordinator::IsDirty(std::size_t index) const {
  return index < state_.open_tabs.size() && TabStateIsDirty(state_.open_tabs[index]);
}

std::vector<std::size_t> TabCoordinator::DirtyIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state_.open_tabs.size());
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
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
  dirty_tabs.reserve(project_state->open_tabs.size());
  for (std::size_t i = 0; i < project_state->open_tabs.size(); ++i) {
    if (TabStateIsDirty(project_state->open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

bool TabCoordinator::ActiveTabIsEditor() const {
  return state_.active_tab_index < state_.open_tabs.size() &&
         state_.open_tabs[state_.active_tab_index].kind == TabEntry::Kind::Editor &&
         state_.open_tabs[state_.active_tab_index].editor_state.has_value();
}

TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.open_tabs[state_.active_tab_index].editor_state.value();
}

const TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.open_tabs[state_.active_tab_index].editor_state.value();
}

editor::TextViewport* TabCoordinator::ActiveEditorViewport() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return &state_.welcome_surface.viewport;
  }
  if (auto* viewport = operations_.find_editor_view(*editor_tab, editor_tab->active_leaf_id);
      viewport != nullptr) {
    return viewport;
  }
  return &editor_tab->views.front().viewport;
}

const editor::TextViewport* TabCoordinator::ActiveEditorViewport() const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return &state_.welcome_surface.viewport;
  }
  const auto it =
      std::find_if(editor_tab->views.begin(), editor_tab->views.end(), [&](const auto& view) {
        return view.leaf_id == editor_tab->active_leaf_id;
      });
  if (it != editor_tab->views.end()) {
    return &it->viewport;
  }
  return &editor_tab->views.front().viewport;
}

void TabCoordinator::Activate(std::size_t index) {
  if (index >= state_.open_tabs.size()) {
    return;
  }

  if (state_.active_tab_index < state_.open_tabs.size() && state_.active_tab_index != index) {
    SyncActiveEditorTab();
  }

  state_.active_tab_index = index;
  auto& tab = state_.open_tabs[index];
  if (tab.kind == TabEntry::Kind::Editor) {
    if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
        return;
      }
      operations_.normalize_editor_split_tree(*tab.editor_state);
      editor::TextViewport* active_view =
          operations_.find_editor_view(*tab.editor_state, tab.editor_state->active_leaf_id);
      if (active_view == nullptr && !tab.editor_state->views.empty()) {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
        active_view = &tab.editor_state->views.front().viewport;
      }
      if (active_view != nullptr) {
        state_.welcome_surface.viewport = *active_view;
        operations_.apply_editor_preferences(state_.welcome_surface.viewport);
      }
    } else {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        return;
      }
      operations_.apply_editor_preferences(loaded_view);
      state_.welcome_surface.viewport = loaded_view;
      tab.editor_state = operations_.make_editor_tab_state(loaded_view);
    }
  }
  SyncActiveEditorTabMetadata();
  if (tab.kind == TabEntry::Kind::Compare) {
    operations_.reveal_active_compare_selection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    operations_.reveal_active_merge_selection();
  } else if (tab.kind == TabEntry::Kind::Editor && !state_.welcome_surface.viewport.path().empty()) {
    state_.directory_tree.SelectPath(state_.welcome_surface.viewport.path().lexically_normal());
    operations_.reveal_selected_tree_sidebar_line();
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(
      tab.kind == TabEntry::Kind::Editor && !state_.welcome_surface.viewport.path().empty());
}

void TabCoordinator::SyncActiveEditorTab() {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return;
  }

  auto& tab = state_.open_tabs[state_.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  if (tab.editor_state->views.empty()) {
    tab.editor_state = operations_.make_editor_tab_state(state_.welcome_surface.viewport);
    return;
  }

  operations_.normalize_editor_split_tree(*tab.editor_state);
  auto* active_view_state =
      FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
  if (active_view_state != nullptr) {
    if (active_view_state->needs_restore) {
      tab.path = operations_.editor_view_path(*active_view_state);
      tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
      return;
    }
    active_view_state->restored_path = active_view_state->viewport.path().lexically_normal();
    active_view_state->restored_cursor_line = active_view_state->viewport.cursor_line();
    active_view_state->restored_cursor_column = active_view_state->viewport.cursor_column();
    active_view_state->restored_scroll_line = active_view_state->viewport.scroll_line();
    active_view_state->restored_horizontal_scroll = active_view_state->viewport.horizontal_scroll();
    active_view_state->needs_restore = false;
  }
  if (state_.active_tab_index < state_.open_tabs.size() &&
      &tab == &state_.open_tabs[state_.active_tab_index]) {
    SyncActiveEditorTabMetadata();
  }
}

bool TabCoordinator::ActivateCurrentTabAfterStateLoad() {
  if (state_.open_tabs.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(state_.active_tab_index, state_.open_tabs.size() - 1);
  state_.active_tab_index = state_.open_tabs.size();
  Activate(active_index);
  return state_.active_tab_index == active_index;
}

void TabCoordinator::SyncActiveEditorTabMetadata() {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return;
  }

  auto& tab = state_.open_tabs[state_.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const editor::TextViewport* viewport = nullptr;
  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    viewport = operations_.find_editor_view(*tab.editor_state, tab.editor_state->active_leaf_id);
    if (viewport == nullptr) {
      viewport = &tab.editor_state->views.front().viewport;
    }
  } else {
    viewport = &state_.welcome_surface.viewport;
  }

  const std::filesystem::path active_path =
      viewport != nullptr ? viewport->path().lexically_normal() : std::filesystem::path{};
  const bool path_changed = tab.path != active_path;
  tab.path = active_path;
  tab.title = viewport != nullptr ? EditorTabLabel(*viewport) : "untitled";
  if (path_changed && !active_path.empty()) {
    state_.directory_tree.SelectPath(active_path);
    operations_.reveal_selected_tree_sidebar_line();
  }
}


void TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  operations_.invalidate_editor_blame_path(path);
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    auto& tab = state_.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() || IsDirty(i)) {
      continue;
    }

    editor::TextViewport reopened_view;
    if (!reopened_view.OpenFile(path)) {
      continue;
    }
    operations_.apply_editor_preferences(reopened_view);

    bool reloaded_any = false;
    for (auto& view : tab.editor_state->views) {
      const std::filesystem::path current_path = operations_.editor_view_path(view);
      if (current_path != path.lexically_normal()) {
        continue;
      }
      const editor::TextViewport* current_view = &view.viewport;
      editor::TextViewport restored_view = reopened_view;
      restored_view.SetViewportSize(current_view->visible_lines(), current_view->visible_columns());
      restored_view.MoveCursorTo(current_view->cursor_line(), current_view->cursor_column());
      restored_view.SetScrollLine(current_view->scroll_line());
      restored_view.SetHorizontalScroll(current_view->horizontal_scroll());
      view.viewport = restored_view;
      view.restored_path = path.lexically_normal();
      view.restored_cursor_line = restored_view.cursor_line();
      view.restored_cursor_column = restored_view.cursor_column();
      view.restored_scroll_line = restored_view.scroll_line();
      view.restored_horizontal_scroll = restored_view.horizontal_scroll();
      view.needs_restore = false;
      reloaded_any = true;
    }
    if (reloaded_any && i == state_.active_tab_index) {
      operations_.normalize_editor_split_tree(*tab.editor_state);
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
  state_.welcome_surface.viewport = untitled_view;

  state_.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
      .editor_state = operations_.make_editor_tab_state(untitled_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.active_tab_index = state_.open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  return true;
}

bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  if (state_.root.empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();

  auto existing = std::find_if(state_.open_tabs.begin(), state_.open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });

  state_.directory_tree.SelectPath(normalized_path);

  if (existing != state_.open_tabs.end()) {
    const std::size_t existing_index =
        static_cast<std::size_t>(std::distance(state_.open_tabs.begin(), existing));
    if (!IsDirty(existing_index)) {
      ReloadCleanEditorTabsForPath(normalized_path);
    }
    Activate(existing_index);
    return true;
  }

  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(opened_view);
  state_.welcome_surface.viewport = opened_view;

  state_.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.active_tab_index = state_.open_tabs.size() - 1;
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

  auto existing = std::find_if(state_.open_tabs.begin(), state_.open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == virtual_path;
                               });
  if (existing != state_.open_tabs.end()) {
    const std::size_t index =
        static_cast<std::size_t>(std::distance(state_.open_tabs.begin(), existing));
    if (!IsDirty(index) && existing->editor_state.has_value()) {
      for (auto& view : existing->editor_state->views) {
        if (operations_.editor_view_path(view) != virtual_path) {
          continue;
        }
        RestoreViewportText(view.viewport, content);
      }
    }
    Activate(index);
    return true;
  }

  editor::TextViewport viewport;
  viewport.LoadContent(content, virtual_path);
  operations_.apply_editor_preferences(viewport);
  state_.welcome_surface.viewport = viewport;

  state_.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = virtual_path,
      .title = std::string(title),
      .editor_state = operations_.make_editor_tab_state(viewport),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.active_tab_index = state_.open_tabs.size() - 1;
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
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    auto& tab = state_.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() || IsDirty(i)) {
      continue;
    }

    bool reloaded_tab = false;
    for (auto& view : tab.editor_state->views) {
      if (operations_.editor_view_path(view) != virtual_path) {
        continue;
      }
      RestoreViewportText(view.viewport, content);
      reloaded_tab = true;
    }
    if (!reloaded_tab) {
      continue;
    }

    reloaded_any = true;
    if (i != state_.active_tab_index) {
      continue;
    }
    operations_.normalize_editor_split_tree(*tab.editor_state);
    auto active_view = std::find_if(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                                    [&](const auto& view) {
                                      return view.leaf_id == tab.editor_state->active_leaf_id;
                                    });
    if (active_view == tab.editor_state->views.end()) {
      continue;
    }
    state_.welcome_surface.viewport = active_view->viewport;
    operations_.apply_editor_preferences(state_.welcome_surface.viewport);
  }

  if (reloaded_any) {
    operations_.request_active_tab_redraw(false);
  }
}

void TabCoordinator::Close(std::size_t index) {
  if (index >= state_.open_tabs.size()) {
    return;
  }
  const bool closing_active = index == state_.active_tab_index;
  const TabEntry& closing_tab = state_.open_tabs[index];

  if (state_.active_tab_index < state_.open_tabs.size() && index != state_.active_tab_index) {
    SyncActiveEditorTab();
  }

  if (closing_tab.kind == TabEntry::Kind::Editor && closing_tab.editor_state.has_value()) {
    for (const auto& view : closing_tab.editor_state->views) {
      const std::filesystem::path path = operations_.editor_view_path(view);
      if (!path.empty() && operations_.count_open_buffer_views(path) == 1) {
        operations_.notify_lsp_buffer_close(path);
      }
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

  state_.open_tabs.erase(state_.open_tabs.begin() + static_cast<std::ptrdiff_t>(index));

  if (state_.open_tabs.empty()) {
    state_.active_tab_index = 0;
    state_.tab_scroll_index = 0;
    state_.welcome_surface.viewport.SetPlaceholderText("microide\n\n"
                                            "Project loaded.\n"
                                            "Use the sidebar to open files.\n");
    state_.surface.focus = FocusTarget::Editor;
    operations_.request_active_tab_redraw(false);
    return;
  }

  if (index < state_.active_tab_index) {
    --state_.active_tab_index;
  } else if (index == state_.active_tab_index) {
    state_.active_tab_index = std::min(index, state_.open_tabs.size() - 1);
    auto& tab = state_.open_tabs[state_.active_tab_index];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        !tab.editor_state->views.empty()) {
      if (EnsureEditorTabLoaded(tab)) {
        operations_.normalize_editor_split_tree(*tab.editor_state);
        if (auto* active_view =
                operations_.find_editor_view(*tab.editor_state, tab.editor_state->active_leaf_id);
            active_view != nullptr) {
          state_.welcome_surface.viewport = *active_view;
          operations_.apply_editor_preferences(state_.welcome_surface.viewport);
        }
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (loaded_view.OpenFile(tab.path)) {
        operations_.apply_editor_preferences(loaded_view);
        state_.welcome_surface.viewport = loaded_view;
        tab.editor_state = operations_.make_editor_tab_state(loaded_view);
      }
    }
    if (!tab.path.empty()) {
      state_.directory_tree.SelectPath(tab.path);
      operations_.reveal_selected_tree_sidebar_line();
    }
    state_.surface.focus = FocusTarget::Editor;
  }

  state_.tab_scroll_index = std::clamp(state_.tab_scroll_index, 0,
                                       std::max(0, static_cast<int>(state_.open_tabs.size()) - 1));
  operations_.ensure_active_tab_visible();
  if (closing_active) {
    operations_.request_active_tab_redraw(!state_.welcome_surface.viewport.path().empty());
  } else {
    operations_.request_tab_strip_redraw();
  }
}

bool TabCoordinator::MoveActiveTo(std::size_t index) {
  if (state_.active_tab_index >= state_.open_tabs.size() || index >= state_.open_tabs.size()) {
    return false;
  }
  if (state_.active_tab_index == index) {
    return true;
  }

  TabEntry moved_tab = std::move(state_.open_tabs[state_.active_tab_index]);
  state_.open_tabs.erase(state_.open_tabs.begin() +
                         static_cast<std::ptrdiff_t>(state_.active_tab_index));
  state_.open_tabs.insert(state_.open_tabs.begin() + static_cast<std::ptrdiff_t>(index),
                          std::move(moved_tab));

  state_.active_tab_index = index;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_tab_strip_redraw();
  return true;
}

bool TabCoordinator::ReopenActive() {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return false;
  }

  auto& tab = state_.open_tabs[state_.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return false;
  }
  const std::filesystem::path reopen_path = state_.welcome_surface.viewport.path().empty()
                                                ? tab.path.lexically_normal()
                                                : state_.welcome_surface.viewport.path().lexically_normal();
  if (reopen_path.empty() || state_.welcome_surface.viewport.dirty()) {
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(reopen_path)) {
    return false;
  }
  operations_.apply_editor_preferences(reopened_view);

  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    operations_.normalize_editor_split_tree(*tab.editor_state);
    for (auto& view : tab.editor_state->views) {
      if (view.leaf_id == tab.editor_state->active_leaf_id ||
          operations_.editor_view_path(view) == reopen_path) {
        view.viewport = reopened_view;
        view.restored_path = reopen_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
      }
    }
    state_.welcome_surface.viewport = reopened_view;
  } else {
    state_.welcome_surface.viewport = reopened_view;
    tab.editor_state = operations_.make_editor_tab_state(reopened_view);
  }
  SyncActiveEditorTabMetadata();
  operations_.invalidate_editor_blame_path(reopen_path);
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

}  // namespace microide::workspace
