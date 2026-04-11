#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include "project/ProjectFileScanner.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kMaxProjectSearchResults = 200;

}  // namespace

WorkspaceShell::FocusTarget WorkspaceShell::PrimarySurfaceFocusTarget() const {
  return surface_.sidebar_visible ? FocusTarget::Sidebar : FocusTarget::Editor;
}

void WorkspaceShell::ShowOverlay(OverlayMode mode) {
  surface_.overlay_visible = true;
  surface_.overlay_mode = mode;
  surface_.focus = FocusTarget::Overlay;
  ResetOverlayScroll();
}

void WorkspaceShell::DismissOverlay(bool focus_editor) {
  surface_.overlay_visible = false;
  surface_.focus = focus_editor ? FocusTarget::Editor : PrimarySurfaceFocusTarget();
}

void WorkspaceShell::OpenBufferSearch() {
  ShowOverlay(OverlayMode::BufferSearch);
  surface_.buffer_search_field = BufferSearchField::Search;
  overlay_workflow_.buffer_search.query.clear();
  overlay_workflow_.buffer_search.replace_text.clear();
  overlay_workflow_.buffer_search.matches.clear();
  overlay_workflow_.buffer_search.selected_index = 0;
}

void WorkspaceShell::OpenBufferReplace() {
  ShowOverlay(OverlayMode::BufferReplace);
  surface_.buffer_search_field = BufferSearchField::Search;
  overlay_workflow_.buffer_search.query.clear();
  overlay_workflow_.buffer_search.replace_text.clear();
  overlay_workflow_.buffer_search.matches.clear();
  overlay_workflow_.buffer_search.selected_index = 0;
}

void WorkspaceShell::OpenProjectSearch() {
  if (project_root_.empty()) {
    return;
  }
  overlay_workflow_.project_search.query.clear();
  overlay_workflow_.project_search.results.clear();
  overlay_workflow_.project_search.selected_index = 0;
  overlay_workflow_.project_search.replace_text.clear();
  ResetOverlayScroll();
  ShowSearchSidebar("", true);
}

void WorkspaceShell::RefreshBufferSearch() {
  overlay_workflow_.buffer_search.matches = FindLiteralSearchMatches(text_viewport_.lines(), overlay_workflow_.buffer_search.query);
  overlay_workflow_.buffer_search.selected_index = 0;

  if (!overlay_workflow_.buffer_search.matches.empty()) {
    const auto& match = overlay_workflow_.buffer_search.matches.front();
    text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  overlay_workflow_.project_search.results.clear();
  overlay_workflow_.project_search.selected_index = 0;
  overlay_workflow_.project_search.truncated = false;
  overlay_workflow_.project_search.error.clear();

  if (project_root_.empty() || overlay_workflow_.project_search.query.empty()) {
    ResetOverlayScroll();
    return;
  }

  overlay_workflow_.project_search.running = true;
  project_search_runtime_.Start(project_root_, overlay_workflow_.project_search.query,
                                overlay_workflow_.project_search.options);
  ResetOverlayScroll();
}

void WorkspaceShell::StopProjectSearch() {
  project_search_runtime_.Stop();
  overlay_workflow_.project_search.running = false;
}

void WorkspaceShell::ConsumeProjectSearchUpdates() {
  const std::optional<project::ProjectSearchUpdate> maybe_update =
      project_search_runtime_.ConsumeActiveUpdate();
  if (!maybe_update.has_value()) {
    return;
  }
  auto update = *maybe_update;

  for (auto& result : update.results) {
    if (overlay_workflow_.project_search.results.size() >= kMaxProjectSearchResults) {
      overlay_workflow_.project_search.truncated = true;
      StopProjectSearch();
      break;
    }
    overlay_workflow_.project_search.results.push_back(std::move(result));
  }

  overlay_workflow_.project_search.truncated = overlay_workflow_.project_search.truncated || update.truncated;
  if (!update.error.empty()) {
    overlay_workflow_.project_search.error = std::move(update.error);
  }
  if (update.finished) {
    overlay_workflow_.project_search.running = false;
  }
  if (surface_.overlay_visible && surface_.overlay_mode == OverlayMode::ProjectSearch &&
      last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ResetOverlayScroll() {
  surface_.overlay_scroll_row = 0;
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (surface_.overlay_mode) {
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

ScrollableListLayout WorkspaceShell::ComputeOverlayListLayout(const SDL_FRect& overlay) const {
  return ComputeScrollableListLayout(overlay, overlay.y + OverlayListStartOffset(),
                                     OverlayItemCount(), surface_.overlay_scroll_row, 18.0f,
                                     22.0f, 18.0f, 16.0f, 8.0f);
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  return ComputeOverlayListLayout(overlay).visible_rows;
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (surface_.overlay_mode) {
    case OverlayMode::CommitPicker:
      return overlay_workflow_.compare_picker.matches.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return overlay_workflow_.buffer_search.matches.size();
    case OverlayMode::ProjectSearch:
      return overlay_workflow_.project_search.results.size();
    case OverlayMode::FileFinder:
    default:
      return file_finder_.results().size();
  }
}

std::size_t WorkspaceShell::OverlaySelectedIndex() const {
  switch (surface_.overlay_mode) {
    case OverlayMode::CommitPicker:
      return overlay_workflow_.compare_picker.selected_index;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return overlay_workflow_.buffer_search.selected_index;
    case OverlayMode::ProjectSearch:
      return overlay_workflow_.project_search.selected_index;
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
  switch (surface_.overlay_mode) {
    case OverlayMode::CommitPicker:
      overlay_workflow_.compare_picker.selected_index = clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      overlay_workflow_.buffer_search.selected_index = clamped_index;
      if (!overlay_workflow_.buffer_search.matches.empty()) {
        const auto& match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      break;
    case OverlayMode::ProjectSearch:
      overlay_workflow_.project_search.selected_index = clamped_index;
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
  surface_.overlay_scroll_row = ComputeOverlayListLayout(overlay).scroll_row;
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const auto layout = ComputeOverlayListLayout(overlay);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  surface_.overlay_scroll_row = RevealScrollableListIndex(layout, selected);
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (surface_.overlay_mode) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      return true;
    case OverlayMode::BufferSearch:
      if (!overlay_workflow_.buffer_search.matches.empty()) {
        const auto& match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      DismissOverlay(true);
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::ProjectSearch:
      if (!overlay_workflow_.project_search.results.empty() &&
          overlay_workflow_.project_search.selected_index < overlay_workflow_.project_search.results.size()) {
        const auto& result = overlay_workflow_.project_search.results[overlay_workflow_.project_search.selected_index];
        OpenFile(project_root_ / result.relative_path);
        text_viewport_.MoveCursorTo(result.line, result.column);
        DismissOverlay(true);
      }
      return true;
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = file_finder_.SelectedPath(); selected.has_value()) {
        OpenFile(project_root_ / *selected);
      }
      DismissOverlay(true);
      return true;
  }
}

void WorkspaceShell::BeginProjectSearchEdit(ProjectSearchEditField field) {
  overlay_workflow_.project_search.edit_field = field;
  overlay_workflow_.project_search.edit_buffer =
      field == ProjectSearchEditField::Query ? overlay_workflow_.project_search.query : overlay_workflow_.project_search.replace_text;
  overlay_workflow_.project_search.editing = true;
}

void WorkspaceShell::CommitProjectSearchEdit() {
  overlay_workflow_.project_search.editing = false;
  if (overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query) {
    overlay_workflow_.project_search.query = overlay_workflow_.project_search.edit_buffer;
    RefreshProjectSearch();
    return;
  }

  overlay_workflow_.project_search.replace_text = overlay_workflow_.project_search.edit_buffer;
}

void WorkspaceShell::CancelProjectSearchEdit() {
  overlay_workflow_.project_search.edit_buffer =
      overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query ? overlay_workflow_.project_search.query
                                                                  : overlay_workflow_.project_search.replace_text;
  overlay_workflow_.project_search.editing = false;
}

SDL_FRect WorkspaceShell::ProjectSearchQueryRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchQueryTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 14.0f);
}

SDL_FRect WorkspaceShell::ProjectSearchReplaceRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchReplaceTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 14.0f);
}

SDL_FRect WorkspaceShell::ProjectSearchModeButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width =
      std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchButtonTop, mode_width,
                  kProjectSearchButtonHeight);
}

SDL_FRect WorkspaceShell::ProjectSearchCaseButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width =
      std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  const float case_width = std::floor(available_width * 0.38f);
  const SDL_FRect mode_rect = ProjectSearchModeButtonRect(sidebar_rect);
  return MakeRect(mode_rect.x + mode_width + gap, mode_rect.y, case_width,
                  kProjectSearchButtonHeight);
}

SDL_FRect WorkspaceShell::ProjectSearchHiddenButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width =
      std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  const float case_width = std::floor(available_width * 0.38f);
  const SDL_FRect case_rect = ProjectSearchCaseButtonRect(sidebar_rect);
  const float hidden_width = std::max(0.0f, available_width - mode_width - case_width);
  return MakeRect(case_rect.x + case_width + gap, case_rect.y, hidden_width,
                  kProjectSearchButtonHeight);
}

std::string WorkspaceShell::ProjectSearchModeButtonLabel() const {
  return overlay_workflow_.project_search.options.pattern_mode == project::ProjectSearchPatternMode::Regex ? "Rx"
                                                                                           : "Lit";
}

std::string WorkspaceShell::ProjectSearchCaseButtonLabel() const {
  switch (overlay_workflow_.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Sensitive:
      return "Case";
    case project::ProjectSearchCaseMode::Insensitive:
      return "NoCase";
    case project::ProjectSearchCaseMode::Smart:
    default:
      return "Smart";
  }
}

std::string WorkspaceShell::ProjectSearchHiddenButtonLabel() const {
  return overlay_workflow_.project_search.options.show_hidden ? "Hide+" : "Hide-";
}

bool WorkspaceShell::ProjectSearchCanReplaceAll() const {
  return overlay_workflow_.project_search.options.pattern_mode == project::ProjectSearchPatternMode::Literal &&
         !overlay_workflow_.project_search.query.empty();
}

bool WorkspaceShell::ProjectSearchReplaceCaseSensitive() const {
  switch (overlay_workflow_.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Sensitive:
      return true;
    case project::ProjectSearchCaseMode::Insensitive:
      return false;
    case project::ProjectSearchCaseMode::Smart:
    default:
      return UsesCaseSensitiveLiteralMatch(overlay_workflow_.project_search.query);
  }
}

void WorkspaceShell::ToggleProjectSearchPatternMode() {
  overlay_workflow_.project_search.options.pattern_mode =
      overlay_workflow_.project_search.options.pattern_mode == project::ProjectSearchPatternMode::Literal
          ? project::ProjectSearchPatternMode::Regex
          : project::ProjectSearchPatternMode::Literal;
  RefreshProjectSearch();
}

void WorkspaceShell::CycleProjectSearchCaseMode() {
  switch (overlay_workflow_.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Smart:
      overlay_workflow_.project_search.options.case_mode = project::ProjectSearchCaseMode::Sensitive;
      break;
    case project::ProjectSearchCaseMode::Sensitive:
      overlay_workflow_.project_search.options.case_mode = project::ProjectSearchCaseMode::Insensitive;
      break;
    case project::ProjectSearchCaseMode::Insensitive:
      overlay_workflow_.project_search.options.case_mode = project::ProjectSearchCaseMode::Smart;
      break;
  }
  RefreshProjectSearch();
}

void WorkspaceShell::ToggleProjectSearchHiddenFiles() {
  overlay_workflow_.project_search.options.show_hidden = !overlay_workflow_.project_search.options.show_hidden;
  RefreshProjectSearch();
}

void WorkspaceShell::ReplaceAllProjectSearchMatches() {
  if (overlay_workflow_.project_search.query.empty()) {
    return;
  }

  if (!ProjectSearchCanReplaceAll()) {
    return;
  }

  const bool case_sensitive = ProjectSearchReplaceCaseSensitive();
  struct PendingProjectReplace {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };

  std::vector<PendingProjectReplace> pending;

  const std::vector<std::filesystem::path> files = project::CollectProjectFiles(
      project_root_, overlay_workflow_.project_search.options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                                         : project::ProjectFileScanMode::ExcludeHidden);
  for (const auto& relative_path : files) {
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
        updated_content, overlay_workflow_.project_search.query, overlay_workflow_.project_search.replace_text, case_sensitive);
    if (replacements == 0) {
      continue;
    }

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      if (open_tabs_[i].kind == TabEntry::Kind::Editor &&
          open_tabs_[i].path.lexically_normal() == normalized_absolute &&
          TabIsDirty(i)) {
        return;
      }
    }
    pending.push_back(PendingProjectReplace{
        .relative_path = relative_path,
        .absolute_path = normalized_absolute,
        .content = std::move(updated_content),
        .replacements = replacements,
    });
  }

  if (pending.empty()) {
    return;
  }

  for (const auto& change : pending) {
    std::ofstream output(change.absolute_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return;
    }
    output.write(change.content.data(), static_cast<std::streamsize>(change.content.size()));
    if (!output.good()) {
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
}

std::vector<int> WorkspaceShell::BuildProjectSearchLineMap() const {
  return BuildProjectSearchResultLineMap(overlay_workflow_.project_search.results);
}

int WorkspaceShell::ProjectSearchLineForResult(std::size_t index) const {
  return FindProjectSearchResultLine(BuildProjectSearchLineMap(), index);
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  if (overlay_workflow_.buffer_search.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(overlay_workflow_.buffer_search.selected_index);
  const int max_index = static_cast<int>(overlay_workflow_.buffer_search.matches.size()) - 1;
  overlay_workflow_.buffer_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  const auto& match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
  text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  if (surface_.overlay_visible && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::MoveProjectSearchSelection(int delta) {
  if (overlay_workflow_.project_search.results.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(overlay_workflow_.project_search.selected_index);
  const int max_index = static_cast<int>(overlay_workflow_.project_search.results.size()) - 1;
  overlay_workflow_.project_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (surface_.overlay_visible && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (overlay_workflow_.buffer_search.matches.empty() ||
      overlay_workflow_.buffer_search.selected_index >= overlay_workflow_.buffer_search.matches.size()) {
    return;
  }

  const auto match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
  if (!text_viewport_.ReplaceRange(match, overlay_workflow_.buffer_search.replace_text)) {
    return;
  }

  RefreshBufferSearch();
  if (!overlay_workflow_.buffer_search.matches.empty()) {
    overlay_workflow_.buffer_search.selected_index =
        std::min(overlay_workflow_.buffer_search.selected_index, overlay_workflow_.buffer_search.matches.size() - 1);
    const auto& next_match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
    text_viewport_.MoveCursorTo(next_match.start.line, next_match.start.column);
  }
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (overlay_workflow_.buffer_search.query.empty()) {
    return;
  }

  text_viewport_.ReplaceAll(overlay_workflow_.buffer_search.query, overlay_workflow_.buffer_search.replace_text);
  RefreshBufferSearch();
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!surface_.overlay_visible || (surface_.overlay_mode != OverlayMode::BufferSearch &&
                            surface_.overlay_mode != OverlayMode::BufferReplace) ||
      overlay_workflow_.buffer_search.matches.empty() ||
      overlay_workflow_.buffer_search.selected_index >= overlay_workflow_.buffer_search.matches.size()) {
    return std::nullopt;
  }
  return overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
}

}  // namespace microide::workspace
