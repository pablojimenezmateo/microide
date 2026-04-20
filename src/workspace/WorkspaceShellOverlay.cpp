#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

WorkspaceShell::FocusTarget WorkspaceShell::PrimarySurfaceFocusTarget() const {
  return sidebar_state_.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
}

void WorkspaceShell::ShowOverlay(OverlayMode mode) {
  RequestOverlayRedraw();
  overlay_state_.visible = true;
  overlay_state_.mode = mode;
  surface_.focus = FocusTarget::Overlay;
  ResetOverlayScroll();
  RequestOverlayRedraw();
}

void WorkspaceShell::DismissOverlay(bool focus_editor) {
  RequestOverlayRedraw();
  overlay_state_.visible = false;
  surface_.focus = focus_editor ? FocusTarget::Editor : PrimarySurfaceFocusTarget();
  RequestOverlayRedraw();
}

void WorkspaceShell::OpenBufferSearch() {
  ShowOverlay(OverlayMode::BufferSearch);
  overlay_state_.buffer_search_field = BufferSearchField::Search;
  overlay_workflow_.buffer_search.query.clear();
  overlay_workflow_.buffer_search.replace_text.clear();
  overlay_workflow_.buffer_search.matches.clear();
  overlay_workflow_.buffer_search.selected_index = 0;
}

void WorkspaceShell::OpenBufferReplace() {
  ShowOverlay(OverlayMode::BufferReplace);
  overlay_state_.buffer_search_field = BufferSearchField::Search;
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

void WorkspaceShell::ResetOverlayScroll() {
  overlay_state_.scroll_row = 0;
  RequestOverlayRedraw();
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (overlay_state_.mode) {
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
                                     OverlayItemCount(), overlay_state_.scroll_row, 18.0f,
                                     22.0f, 18.0f, 16.0f, 8.0f);
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  return ComputeOverlayListLayout(overlay).visible_rows;
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (overlay_state_.mode) {
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
  switch (overlay_state_.mode) {
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
  switch (overlay_state_.mode) {
    case OverlayMode::CommitPicker:
      overlay_workflow_.compare_picker.selected_index = clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      overlay_workflow_.buffer_search.selected_index = clamped_index;
      if (!overlay_workflow_.buffer_search.matches.empty()) {
        const auto& match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(match.start.line, match.start.column);
        }
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
  RequestOverlayRedraw();
  if (overlay_state_.mode == OverlayMode::BufferSearch ||
      overlay_state_.mode == OverlayMode::BufferReplace) {
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::ClampOverlayScrollRow(const SDL_FRect& overlay) {
  overlay_state_.scroll_row = ComputeOverlayListLayout(overlay).scroll_row;
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const auto layout = ComputeOverlayListLayout(overlay);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  overlay_state_.scroll_row = RevealScrollableListIndex(layout, selected);
  RequestOverlayRedraw();
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (overlay_state_.mode) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      return true;
    case OverlayMode::BufferSearch:
      if (!overlay_workflow_.buffer_search.matches.empty()) {
        const auto& match = overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(match.start.line, match.start.column);
        }
      }
      DismissOverlay(true);
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::ProjectSearch:
      if (!overlay_workflow_.project_search.results.empty() &&
          overlay_workflow_.project_search.selected_index <
              overlay_workflow_.project_search.results.size()) {
        const auto& result =
            overlay_workflow_.project_search.results[overlay_workflow_.project_search.selected_index];
        OpenFile(project_root_ / result.relative_path);
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(result.line, result.column);
        }
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

}  // namespace microide::workspace
