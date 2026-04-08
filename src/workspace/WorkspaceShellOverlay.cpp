#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kMaxProjectSearchResults = 200;

}  // namespace

void WorkspaceShell::OpenBufferSearch() {
  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::BufferSearch;
  buffer_search_field_ = BufferSearchField::Search;
  focus_ = FocusTarget::Overlay;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  ResetOverlayScroll();
  LogMessage("Buffer search opened");
}

void WorkspaceShell::OpenBufferReplace() {
  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::BufferReplace;
  buffer_search_field_ = BufferSearchField::Search;
  focus_ = FocusTarget::Overlay;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  ResetOverlayScroll();
  LogMessage("Buffer replace opened");
}

void WorkspaceShell::OpenProjectSearch() {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return;
  }
  project_search_query_.clear();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_replace_text_.clear();
  ResetOverlayScroll();
  ShowSearchSidebar("", true);
}

void WorkspaceShell::RefreshBufferSearch() {
  buffer_search_matches_ = FindLiteralSearchMatches(text_viewport_.lines(), buffer_search_query_);
  buffer_search_selected_index_ = 0;

  if (!buffer_search_matches_.empty()) {
    const auto& match = buffer_search_matches_.front();
    text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_search_error_.clear();

  if (project_root_.empty() || project_search_query_.empty()) {
    ResetOverlayScroll();
    return;
  }

  project_search_running_ = true;
  project_search_run_id_ =
      project_search_service_.Start(project_root_, project_search_query_, false);
  ResetOverlayScroll();
}

void WorkspaceShell::StopProjectSearch() {
  project_search_service_.Stop();
  project_search_running_ = false;
  project_search_run_id_ = 0;
}

void WorkspaceShell::ConsumeProjectSearchUpdates() {
  auto update = project_search_service_.TakePendingUpdate();
  if (update.run_id == 0 || update.run_id != project_search_run_id_) {
    return;
  }

  for (auto& result : update.results) {
    if (project_search_results_.size() >= kMaxProjectSearchResults) {
      StopProjectSearch();
      break;
    }
    project_search_results_.push_back(std::move(result));
  }

  if (!update.error.empty()) {
    project_search_error_ = std::move(update.error);
  }
  if (update.finished) {
    project_search_running_ = false;
  }
  if (overlay_visible_ && overlay_mode_ == OverlayMode::ProjectSearch &&
      last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ResetOverlayScroll() {
  overlay_scroll_row_ = 0;
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (overlay_mode_) {
    case OverlayMode::FileFinder:
      return 74.0f;
    case OverlayMode::BufferReplace:
      return 106.0f;
    case OverlayMode::BufferSearch:
    case OverlayMode::ProjectSearch:
    case OverlayMode::CommitPicker:
    default:
      return 86.0f;
  }
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  constexpr float kOverlayRowHeight = 22.0f;
  const float available_height = overlay.h - OverlayListStartOffset() - 16.0f;
  return std::max(1, static_cast<int>(std::floor(std::max(0.0f, available_height) / kOverlayRowHeight)));
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      return compare_picker_matches_.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return buffer_search_matches_.size();
    case OverlayMode::ProjectSearch:
      return project_search_results_.size();
    case OverlayMode::FileFinder:
    default:
      return file_finder_.results().size();
  }
}

std::size_t WorkspaceShell::OverlaySelectedIndex() const {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      return compare_picker_selected_index_;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return buffer_search_selected_index_;
    case OverlayMode::ProjectSearch:
      return project_search_selected_index_;
    case OverlayMode::FileFinder:
    default:
      return file_finder_.selected_index();
  }
}

void WorkspaceShell::SetOverlaySelectedIndex(std::size_t index) {
  const std::size_t item_count = OverlayItemCount();
  if (item_count == 0) {
    return;
  }
  const std::size_t clamped_index = std::min(index, item_count - 1);
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      compare_picker_selected_index_ = clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      buffer_search_selected_index_ = clamped_index;
      if (!buffer_search_matches_.empty()) {
        const auto& match = buffer_search_matches_[buffer_search_selected_index_];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      break;
    case OverlayMode::ProjectSearch:
      project_search_selected_index_ = clamped_index;
      break;
    case OverlayMode::FileFinder:
    default: {
      const std::size_t current_index = file_finder_.selected_index();
      file_finder_.MoveSelection(static_cast<int>(clamped_index) - static_cast<int>(current_index));
      break;
    }
  }
}

void WorkspaceShell::ClampOverlayScrollRow(const SDL_FRect& overlay) {
  const int visible_rows = OverlayVisibleRows(overlay);
  const int max_scroll = std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
  overlay_scroll_row_ = std::clamp(overlay_scroll_row_, 0, max_scroll);
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const int visible_rows = OverlayVisibleRows(overlay);
  const int max_scroll = std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  if (selected < overlay_scroll_row_) {
    overlay_scroll_row_ = selected;
  } else if (selected >= overlay_scroll_row_ + visible_rows) {
    overlay_scroll_row_ = selected - visible_rows + 1;
  }
  overlay_scroll_row_ = std::clamp(overlay_scroll_row_, 0, max_scroll);
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      return true;
    case OverlayMode::BufferSearch:
      if (!buffer_search_matches_.empty()) {
        const auto& match = buffer_search_matches_[buffer_search_selected_index_];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      overlay_visible_ = false;
      focus_ = FocusTarget::Editor;
      LogMessage("Buffer search closed");
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::ProjectSearch:
      if (!project_search_results_.empty() &&
          project_search_selected_index_ < project_search_results_.size()) {
        const auto& result = project_search_results_[project_search_selected_index_];
        OpenFile(project_root_ / result.relative_path);
        text_viewport_.MoveCursorTo(result.line, result.column);
        overlay_visible_ = false;
        focus_ = FocusTarget::Editor;
        LogMessage("Project search result opened");
      }
      return true;
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = file_finder_.SelectedPath(); selected.has_value()) {
        OpenFile(project_root_ / *selected);
      }
      overlay_visible_ = false;
      focus_ = FocusTarget::Editor;
      LogMessage("Finder selection opened");
      return true;
  }
}

void WorkspaceShell::BeginProjectSearchEdit(ProjectSearchEditField field) {
  project_search_edit_field_ = field;
  project_search_edit_buffer_ =
      field == ProjectSearchEditField::Query ? project_search_query_ : project_replace_text_;
  project_search_editing_ = true;
}

void WorkspaceShell::CommitProjectSearchEdit() {
  project_search_editing_ = false;
  if (project_search_edit_field_ == ProjectSearchEditField::Query) {
    project_search_query_ = project_search_edit_buffer_;
    RefreshProjectSearch();
    LogMessage("Project search updated");
    return;
  }

  project_replace_text_ = project_search_edit_buffer_;
  LogMessage("Project replacement text updated");
}

void WorkspaceShell::CancelProjectSearchEdit() {
  project_search_edit_buffer_ =
      project_search_edit_field_ == ProjectSearchEditField::Query ? project_search_query_
                                                                  : project_replace_text_;
  project_search_editing_ = false;
  LogMessage("Project search edit cancelled");
}

void WorkspaceShell::ReplaceAllProjectSearchMatches() {
  if (project_search_query_.empty()) {
    LogMessage("Project replace needs a search query");
    return;
  }

  if (!QuerySupportsLiteralReplace(project_search_query_)) {
    LogMessage("Project replace currently supports literal queries only");
    return;
  }

  const bool case_sensitive = UsesCaseSensitiveLiteralMatch(project_search_query_);
  struct PendingProjectReplace {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };

  std::vector<PendingProjectReplace> pending;
  std::size_t replaced_total = 0;

  for (const auto& relative_path : file_index_.files()) {
    const std::filesystem::path absolute_path = project_root_ / relative_path;
    const std::filesystem::path normalized_absolute = absolute_path.lexically_normal();

    std::ifstream input(absolute_path, std::ios::binary);
    if (!input) {
      continue;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();
    if (content.find('\0') != std::string::npos) {
      continue;
    }

    std::string updated_content = content;
    const std::size_t replacements = ReplaceLiteralMatchesInText(
        updated_content, project_search_query_, project_replace_text_, case_sensitive);
    if (replacements == 0) {
      continue;
    }

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      if (open_tabs_[i].kind == TabEntry::Kind::Editor &&
          open_tabs_[i].path.lexically_normal() == normalized_absolute &&
          TabIsDirty(i)) {
        LogMessage("Project replace blocked by dirty tab: " + relative_path.string());
        return;
      }
    }
    replaced_total += replacements;
    pending.push_back(PendingProjectReplace{
        .relative_path = relative_path,
        .absolute_path = normalized_absolute,
        .content = std::move(updated_content),
        .replacements = replacements,
    });
  }

  if (pending.empty()) {
    LogMessage("Project replace found no literal matches");
    return;
  }

  for (const auto& change : pending) {
    std::ofstream output(change.absolute_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      LogMessage("Project replace failed to write: " + change.relative_path.string());
      return;
    }
    output.write(change.content.data(), static_cast<std::streamsize>(change.content.size()));
    if (!output.good()) {
      LogMessage("Project replace failed to write: " + change.relative_path.string());
      return;
    }

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      auto& tab = open_tabs_[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      editor::TextViewport reopened_view;
      if (!reopened_view.OpenFile(change.absolute_path)) {
        continue;
      }
      ApplyEditorPreferences(reopened_view);
      bool reloaded_any = false;
      for (auto& view : tab.editor_state->views) {
        const bool active_view =
            i == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
            !view.needs_restore;
        const std::filesystem::path current_path =
            active_view ? text_viewport_.path().lexically_normal() : EditorViewPath(view);
        if (current_path != change.absolute_path) {
          continue;
        }
        view.viewport = reopened_view;
        view.restored_path = change.absolute_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
        if (active_view) {
          text_viewport_ = reopened_view;
        }
        reloaded_any = true;
      }
      if (reloaded_any && i == active_tab_index_) {
        NormalizeEditorSplitTree(*tab.editor_state);
        SyncActiveEditorTabMetadata();
      }
    }
  }

  RefreshProjectFiles();
  RefreshProjectSearch();
  LogMessage("Replaced " + std::to_string(replaced_total) + " matches in " +
             std::to_string(pending.size()) + " files");
}

std::vector<int> WorkspaceShell::BuildProjectSearchLineMap() const {
  return BuildProjectSearchResultLineMap(project_search_results_);
}

int WorkspaceShell::ProjectSearchLineForResult(std::size_t index) const {
  return FindProjectSearchResultLine(BuildProjectSearchLineMap(), index);
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  if (buffer_search_matches_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(buffer_search_selected_index_);
  const int max_index = static_cast<int>(buffer_search_matches_.size()) - 1;
  buffer_search_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  const auto& match = buffer_search_matches_[buffer_search_selected_index_];
  text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::MoveProjectSearchSelection(int delta) {
  if (project_search_results_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(project_search_selected_index_);
  const int max_index = static_cast<int>(project_search_results_.size()) - 1;
  project_search_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (buffer_search_matches_.empty() ||
      buffer_search_selected_index_ >= buffer_search_matches_.size()) {
    return;
  }

  const auto match = buffer_search_matches_[buffer_search_selected_index_];
  if (!text_viewport_.ReplaceRange(match, buffer_replace_text_)) {
    return;
  }

  RefreshBufferSearch();
  if (!buffer_search_matches_.empty()) {
    buffer_search_selected_index_ =
        std::min(buffer_search_selected_index_, buffer_search_matches_.size() - 1);
    const auto& next_match = buffer_search_matches_[buffer_search_selected_index_];
    text_viewport_.MoveCursorTo(next_match.start.line, next_match.start.column);
  }
  LogMessage("Replaced current match");
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (buffer_search_query_.empty()) {
    return;
  }

  const std::size_t replaced =
      text_viewport_.ReplaceAll(buffer_search_query_, buffer_replace_text_);
  RefreshBufferSearch();
  LogMessage("Replaced " + std::to_string(replaced) + " matches");
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!overlay_visible_ || (overlay_mode_ != OverlayMode::BufferSearch &&
                            overlay_mode_ != OverlayMode::BufferReplace) ||
      buffer_search_matches_.empty() ||
      buffer_search_selected_index_ >= buffer_search_matches_.size()) {
    return std::nullopt;
  }
  return buffer_search_matches_[buffer_search_selected_index_];
}

}  // namespace microide::workspace
