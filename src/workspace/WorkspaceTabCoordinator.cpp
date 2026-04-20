#include "workspace/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "welcome" : "untitled";
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
    return EditorTabLabel(state_.text_viewport);
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
        SerializeLines(merge_tab.result_viewport.lines(), merge_tab.result_line_ending);
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
  std::vector<std::filesystem::path> saved_paths;
  for (auto& view : editor_state->views) {
    editor::TextViewport* candidate = &view.viewport;
    if (candidate->path().empty()) {
      if (candidate->dirty()) {
        return false;
      }
      continue;
    }
    if (!candidate->dirty()) {
      continue;
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
  }
  return attempted_save || !editor_state->views.empty();
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
      operations_.sync_active_editor_tab_metadata();
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
  state_.text_viewport = untitled_view;

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
    operations_.activate_tab(existing_index);
    return true;
  }

  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(opened_view);
  state_.text_viewport = opened_view;

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

std::optional<std::size_t> TabCoordinator::FindIndexBySpecifier(std::string_view specifier,
                                                                std::string* error_message) const {
  if (specifier.empty()) {
    if (error_message != nullptr) {
      *error_message = "usage: tabswitch <tab>";
    }
    return std::nullopt;
  }

  const std::string lowered_specifier = ToLower(specifier);
  try {
    std::size_t parsed_length = 0;
    const int tab_number = std::stoi(std::string(specifier), &parsed_length);
    if (parsed_length == specifier.size()) {
      if (tab_number >= 1 && static_cast<std::size_t>(tab_number) <= state_.open_tabs.size()) {
        return static_cast<std::size_t>(tab_number - 1);
      }
      if (error_message != nullptr) {
        *error_message = "Invalid tab index";
      }
      return std::nullopt;
    }
  } catch (...) {
  }

  std::vector<std::size_t> exact_matches;
  std::vector<std::size_t> partial_matches;
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    const TabEntry& tab = state_.open_tabs[i];
    const std::string lowered_title = ToLower(tab.title);
    const std::string lowered_path = ToLower(RelativePathLabel(state_.root, tab.path));
    const std::string lowered_absolute_path = ToLower(tab.path.lexically_normal().string());
    const bool exact_match = lowered_title == lowered_specifier ||
                             (!lowered_path.empty() && lowered_path == lowered_specifier) ||
                             (!lowered_absolute_path.empty() &&
                              lowered_absolute_path == lowered_specifier);
    const bool partial_match = lowered_title.find(lowered_specifier) != std::string::npos ||
                               (!lowered_path.empty() &&
                                lowered_path.find(lowered_specifier) != std::string::npos) ||
                               (!lowered_absolute_path.empty() &&
                                lowered_absolute_path.find(lowered_specifier) != std::string::npos);
    if (exact_match) {
      exact_matches.push_back(i);
    } else if (partial_match) {
      partial_matches.push_back(i);
    }
  }

  if (exact_matches.size() == 1) {
    return exact_matches.front();
  }
  if (exact_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (error_message != nullptr) {
    *error_message = "Unknown tab: " + std::string(specifier);
  }
  return std::nullopt;
}

bool TabCoordinator::ReopenActive() {
  if (state_.active_tab_index >= state_.open_tabs.size()) {
    return false;
  }

  auto& tab = state_.open_tabs[state_.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return false;
  }
  const std::filesystem::path reopen_path = state_.text_viewport.path().empty()
                                                ? tab.path.lexically_normal()
                                                : state_.text_viewport.path().lexically_normal();
  if (reopen_path.empty() || state_.text_viewport.dirty()) {
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
    state_.text_viewport = reopened_view;
  } else {
    state_.text_viewport = reopened_view;
    tab.editor_state = operations_.make_editor_tab_state(reopened_view);
  }
  operations_.sync_active_editor_tab_metadata();
  operations_.invalidate_editor_blame_path(reopen_path);
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

TabCoordinator WorkspaceShell::MakeTabCoordinator() {
  return TabCoordinator(
      context_.project_catalog,
      context_.current_project_state,
      TabCoordinator::Operations{
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .notify_plugin_buffer_save =
              [this](const std::filesystem::path& path) { NotifyPluginBufferSave(path); },
          .notify_plugin_buffer_open =
              [this](const std::filesystem::path& path) { NotifyPluginBufferOpen(path); },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .make_editor_tab_state =
              [this](const editor::TextViewport& viewport) { return MakeEditorTabState(viewport); },
          .editor_view_path =
              [this](const TabEntry::EditorTabState::EditorViewState& view) {
                return EditorViewPath(view);
              },
          .normalize_editor_split_tree =
              [this](TabEntry::EditorTabState& editor_tab) { NormalizeEditorSplitTree(editor_tab); },
          .sync_active_editor_tab_metadata = [this]() { SyncActiveEditorTabMetadata(); },
          .ensure_active_tab_visible = [this]() { EnsureActiveTabVisible(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_tab_redraw =
              [this](bool include_tree_sidebar) { RequestActiveTabRedraw(include_tree_sidebar); },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .activate_tab = [this](std::size_t index) { ActivateTab(index); },
      });
}

std::string WorkspaceShell::ActiveTabTitle() const {
  return const_cast<WorkspaceShell*>(this)->MakeTabCoordinator().ActiveTitle();
}

bool WorkspaceShell::SaveTab(std::size_t index) {
  return MakeTabCoordinator().Save(index);
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  return const_cast<WorkspaceShell*>(this)->MakeTabCoordinator().IsDirty(index);
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = context_.current_project_state.open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(context_.current_project_state.root, path, tab.title, TabIsDirty(index)).display_title;
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t index) const {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = context_.current_project_state.open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(context_.current_project_state.root, path, tab.title, TabIsDirty(index)).tooltip_label;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  return const_cast<WorkspaceShell*>(this)->MakeTabCoordinator().DirtyIndices();
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.open_tabs.size());
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    if (TabCoordinator::TabStateIsDirty(state.open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)->MakeTabCoordinator().DirtyIndicesForProject(project_index);
}

void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  MakeTabCoordinator().ReloadCleanEditorTabsForPath(path);
}

bool WorkspaceShell::OpenUntitledTab() {
  return MakeTabCoordinator().OpenUntitled();
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  return MakeTabCoordinator().OpenFileInNewTab(path);
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  return MakeTabCoordinator().MoveActiveTo(index);
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return const_cast<WorkspaceShell*>(this)->MakeTabCoordinator().FindIndexBySpecifier(specifier,
                                                                                       error_message);
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  if (!OpenFileInNewTab(path)) {
    return;
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  return MakeTabCoordinator().ReopenActive();
}

}  // namespace microide::workspace
