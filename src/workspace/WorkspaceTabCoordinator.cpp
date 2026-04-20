#include "workspace/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
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

TabCoordinator::TabCoordinator(WorkspaceShell& shell) : shell_(shell) {}

std::string TabCoordinator::ActiveTitle() const {
  if (shell_.active_tab_index_ >= shell_.open_tabs_.size()) {
    return EditorTabLabel(shell_.text_viewport_);
  }
  return shell_.open_tabs_[shell_.active_tab_index_].title;
}

bool TabCoordinator::Save(std::size_t index) {
  if (index >= shell_.open_tabs_.size()) {
    return false;
  }

  if (shell_.open_tabs_[index].kind == WorkspaceShell::TabEntry::Kind::Compare &&
      shell_.open_tabs_[index].compare.has_value()) {
    auto& compare_tab = shell_.open_tabs_[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    if (!compare_tab.right_viewport.Save()) {
      return false;
    }
    shell_.directory_tree_.Refresh();
    shell_.NotifyPluginBufferSave(compare_tab.right_viewport.path());
    return true;
  }

  if (shell_.open_tabs_[index].kind == WorkspaceShell::TabEntry::Kind::Merge &&
      shell_.open_tabs_[index].merge.has_value()) {
    auto& merge_tab = shell_.open_tabs_[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (!merge_tab.result_viewport.Save()) {
      return false;
    }
    merge_tab.persisted_output_baseline =
        SerializeLines(merge_tab.result_viewport.lines(), merge_tab.result_line_ending);
    shell_.directory_tree_.Refresh();
    shell_.NotifyPluginBufferSave(merge_tab.result_viewport.path());
    return true;
  }

  if (shell_.open_tabs_[index].kind != WorkspaceShell::TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = shell_.open_tabs_[index].editor_state;
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
      shell_.InvalidateEditorBlamePath(path);
      shell_.NotifyPluginBufferSave(path);
    }
    shell_.directory_tree_.Refresh();
  }
  return attempted_save || !editor_state->views.empty();
}

bool TabCoordinator::IsDirty(std::size_t index) const {
  if (index >= shell_.open_tabs_.size()) {
    return false;
  }

  if (shell_.open_tabs_[index].kind == WorkspaceShell::TabEntry::Kind::Compare &&
      shell_.open_tabs_[index].compare.has_value()) {
    return shell_.open_tabs_[index].compare->right_editable &&
           shell_.open_tabs_[index].compare->right_viewport.dirty();
  }

  if (shell_.open_tabs_[index].kind == WorkspaceShell::TabEntry::Kind::Merge &&
      shell_.open_tabs_[index].merge.has_value()) {
    return shell_.open_tabs_[index].merge->result_viewport.dirty();
  }

  if (shell_.open_tabs_[index].kind != WorkspaceShell::TabEntry::Kind::Editor) {
    return false;
  }

  const auto& editor_state = shell_.open_tabs_[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  for (const auto& view : editor_state->views) {
    if (view.viewport.dirty()) {
      return true;
    }
  }
  return false;
}

std::vector<std::size_t> TabCoordinator::DirtyIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(shell_.open_tabs_.size());
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    if (IsDirty(i)) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> TabCoordinator::DirtyIndicesForProject(std::size_t project_index) const {
  if (project_index >= shell_.project_catalog_.entries.size()) {
    return {};
  }
  if (shell_.HasActiveProjectCatalogEntry() && project_index == shell_.project_catalog_.active_index) {
    return DirtyIndices();
  }
  const auto* state = shell_.ProjectCatalogEntry(project_index);
  return state == nullptr ? std::vector<std::size_t>{}
                          : WorkspaceShell::DirtyEditorTabIndices(*state);
}

void TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  shell_.InvalidateEditorBlamePath(path);
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    auto& tab = shell_.open_tabs_[i];
    if (tab.kind != WorkspaceShell::TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        IsDirty(i)) {
      continue;
    }

    editor::TextViewport reopened_view;
    if (!reopened_view.OpenFile(path)) {
      continue;
    }
    shell_.ApplyEditorPreferences(reopened_view);

    bool reloaded_any = false;
      for (auto& view : tab.editor_state->views) {
      const std::filesystem::path current_path = shell_.EditorViewPath(view);
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
    if (reloaded_any && i == shell_.active_tab_index_) {
      shell_.NormalizeEditorSplitTree(*tab.editor_state);
      shell_.SyncActiveEditorTabMetadata();
      shell_.RequestEditorSurfaceRedraw();
    }
  }
}

bool TabCoordinator::OpenUntitled() {
  if (shell_.project_root_.empty()) {
    return false;
  }
  shell_.SyncActiveEditorTab();

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  shell_.ApplyEditorPreferences(untitled_view);
  shell_.text_viewport_ = untitled_view;

  shell_.open_tabs_.push_back(WorkspaceShell::TabEntry{
      .kind = WorkspaceShell::TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
      .editor_state = shell_.MakeEditorTabState(untitled_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  shell_.active_tab_index_ = shell_.open_tabs_.size() - 1;
  shell_.EnsureActiveTabVisible();
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  shell_.ResetCaretBlink();
  shell_.RequestActiveTabRedraw(false);
  return true;
}

bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  if (shell_.project_root_.empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();
  shell_.SyncActiveEditorTab();

  auto existing = std::find_if(shell_.open_tabs_.begin(), shell_.open_tabs_.end(),
                               [&](const WorkspaceShell::TabEntry& tab) {
                                 return tab.kind == WorkspaceShell::TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });

  shell_.directory_tree_.SelectPath(normalized_path);
  shell_.RevealSelectedTreeSidebarLine();

  if (existing != shell_.open_tabs_.end()) {
    const std::size_t existing_index =
        static_cast<std::size_t>(std::distance(shell_.open_tabs_.begin(), existing));
    if (!IsDirty(existing_index)) {
      ReloadCleanEditorTabsForPath(normalized_path);
    }
    shell_.ActivateTab(existing_index);
    return true;
  }

  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(normalized_path)) {
    return false;
  }
  shell_.ApplyEditorPreferences(opened_view);
  shell_.text_viewport_ = opened_view;

  shell_.open_tabs_.push_back(WorkspaceShell::TabEntry{
      .kind = WorkspaceShell::TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = shell_.MakeEditorTabState(opened_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  shell_.active_tab_index_ = shell_.open_tabs_.size() - 1;
  shell_.EnsureActiveTabVisible();
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  shell_.ResetCaretBlink();
  shell_.NotifyPluginBufferOpen(normalized_path);
  shell_.RequestActiveTabRedraw(true);
  return true;
}

bool TabCoordinator::MoveActiveTo(std::size_t index) {
  if (shell_.active_tab_index_ >= shell_.open_tabs_.size() || index >= shell_.open_tabs_.size()) {
    return false;
  }

  if (shell_.active_tab_index_ == index) {
    return true;
  }

  shell_.SyncActiveEditorTab();

  WorkspaceShell::TabEntry moved_tab = std::move(shell_.open_tabs_[shell_.active_tab_index_]);
  shell_.open_tabs_.erase(
      shell_.open_tabs_.begin() + static_cast<std::ptrdiff_t>(shell_.active_tab_index_));
  shell_.open_tabs_.insert(shell_.open_tabs_.begin() + static_cast<std::ptrdiff_t>(index),
                           std::move(moved_tab));

  shell_.active_tab_index_ = index;
  shell_.EnsureActiveTabVisible();
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  shell_.RequestTabStripRedraw();
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
      if (tab_number >= 1 && static_cast<std::size_t>(tab_number) <= shell_.open_tabs_.size()) {
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
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const WorkspaceShell::TabEntry& tab = shell_.open_tabs_[i];
    const std::string lowered_title = ToLower(tab.title);
    const std::string lowered_path = ToLower(RelativePathLabel(shell_.project_root_, tab.path));
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
  if (shell_.active_tab_index_ >= shell_.open_tabs_.size()) {
    return false;
  }

  auto& tab = shell_.open_tabs_[shell_.active_tab_index_];
  if (tab.kind != WorkspaceShell::TabEntry::Kind::Editor) {
    return false;
  }
  const std::filesystem::path reopen_path = shell_.text_viewport_.path().empty()
                                                ? tab.path.lexically_normal()
                                                : shell_.text_viewport_.path().lexically_normal();
  if (reopen_path.empty()) {
    return false;
  }
  if (shell_.text_viewport_.dirty()) {
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(reopen_path)) {
    return false;
  }
  shell_.ApplyEditorPreferences(reopened_view);

  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    shell_.NormalizeEditorSplitTree(*tab.editor_state);
    for (auto& view : tab.editor_state->views) {
      if (view.leaf_id == tab.editor_state->active_leaf_id ||
          shell_.EditorViewPath(view) == reopen_path) {
        view.viewport = reopened_view;
        view.restored_path = reopen_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
      }
    }
    shell_.text_viewport_ = reopened_view;
  } else {
    shell_.text_viewport_ = reopened_view;
    tab.editor_state = shell_.MakeEditorTabState(reopened_view);
  }
  shell_.SyncActiveEditorTabMetadata();
  shell_.InvalidateEditorBlamePath(reopen_path);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  shell_.ResetCaretBlink();
  shell_.RequestEditorSurfaceRedraw();
  return true;
}

std::string WorkspaceShell::ActiveTabTitle() const {
  return TabCoordinator(*const_cast<WorkspaceShell*>(this)).ActiveTitle();
}

bool WorkspaceShell::SaveTab(std::size_t index) {
  return TabCoordinator(*this).Save(index);
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  return TabCoordinator(*const_cast<WorkspaceShell*>(this)).IsDirty(index);
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return {};
  }

  const TabEntry& tab = open_tabs_[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(project_root_, path, tab.title, TabIsDirty(index)).display_title;
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return {};
  }

  const TabEntry& tab = open_tabs_[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(project_root_, path, tab.title, TabIsDirty(index)).tooltip_label;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  return TabCoordinator(*const_cast<WorkspaceShell*>(this)).DirtyIndices();
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.open_tabs.size());
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const auto& tab = state.open_tabs[i];
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_editable && tab.compare->right_viewport.dirty()) {
      dirty_tabs.push_back(i);
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.dirty()) {
      dirty_tabs.push_back(i);
      continue;
    }
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        tab.editor_state->views.empty()) {
      continue;
    }
    const bool dirty = std::any_of(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                                   [](const auto& view) { return view.viewport.dirty(); });
    if (dirty) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  return TabCoordinator(*const_cast<WorkspaceShell*>(this)).DirtyIndicesForProject(project_index);
}

void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  TabCoordinator(*this).ReloadCleanEditorTabsForPath(path);
}

bool WorkspaceShell::OpenUntitledTab() {
  return TabCoordinator(*this).OpenUntitled();
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  return TabCoordinator(*this).OpenFileInNewTab(path);
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  return TabCoordinator(*this).MoveActiveTo(index);
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return TabCoordinator(*const_cast<WorkspaceShell*>(this)).FindIndexBySpecifier(specifier,
                                                                                  error_message);
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  if (!OpenFileInNewTab(path)) {
    return;
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  return TabCoordinator(*this).ReopenActive();
}

}  // namespace microide::workspace
