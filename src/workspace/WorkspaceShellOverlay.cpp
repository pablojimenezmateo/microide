#include "workspace/WorkspaceShell.h"

#include <algorithm>


namespace microide::workspace {

WorkspaceShell::FocusTarget WorkspaceShell::PrimarySurfaceFocusTarget() const {
  return context_.current_project_state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
}

void WorkspaceShell::ShowOverlay(OverlayMode mode) {
  RequestOverlayRedraw();
  context_.current_project_state.overlay.visible = true;
  context_.current_project_state.overlay.mode = mode;
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  ResetOverlayScroll();
  RequestOverlayRedraw();
}

void WorkspaceShell::DismissOverlay(bool focus_editor) {
  RequestOverlayRedraw();
  context_.current_project_state.overlay.visible = false;
  context_.current_project_state.surface.focus = focus_editor ? FocusTarget::Editor : PrimarySurfaceFocusTarget();
  RequestOverlayRedraw();
}

void WorkspaceShell::OpenBufferSearch() {
  ShowOverlay(OverlayMode::BufferSearch);
  context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
  context_.current_project_state.overlay.workflow.buffer_search.query.SetText("");
  context_.current_project_state.overlay.workflow.buffer_search.replace_text.SetText("");
  context_.current_project_state.overlay.workflow.buffer_search.matches.clear();
  context_.current_project_state.overlay.workflow.buffer_search.selected_index = 0;
}

void WorkspaceShell::OpenBufferReplace() {
  ShowOverlay(OverlayMode::BufferReplace);
  context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
  context_.current_project_state.overlay.workflow.buffer_search.query.SetText("");
  context_.current_project_state.overlay.workflow.buffer_search.replace_text.SetText("");
  context_.current_project_state.overlay.workflow.buffer_search.matches.clear();
  context_.current_project_state.overlay.workflow.buffer_search.selected_index = 0;
}

void WorkspaceShell::OpenProjectSearch() {
  if (context_.current_project_state.root.empty()) {
    return;
  }
  context_.current_project_state.overlay.workflow.project_search.query.SetText("");
  context_.current_project_state.overlay.workflow.project_search.results.clear();
  context_.current_project_state.overlay.workflow.project_search.selected_index = 0;
  context_.current_project_state.overlay.workflow.project_search.replace_text.SetText("");
  ResetOverlayScroll();
  ShowSearchSidebar("", true);
}

void WorkspaceShell::ResetOverlayScroll() {
  context_.current_project_state.overlay.scroll_row = 0;
  RequestOverlayRedraw();
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::FileFinder:
      return 74.0f;
    case OverlayMode::BufferReplace:
      return 106.0f;
    case OverlayMode::BufferSearch:
    case OverlayMode::ProjectSearch:
    case OverlayMode::CommitPicker:
    case OverlayMode::Completion:
    case OverlayMode::CodeActions:
    default:
      return 86.0f;
  }
}

ScrollableListLayout WorkspaceShell::ComputeOverlayListLayout(const SDL_FRect& overlay) const {
  return ComputeScrollableListLayout(overlay, overlay.y + OverlayListStartOffset(),
                                     OverlayItemCount(), context_.current_project_state.overlay.scroll_row, 18.0f,
                                     22.0f, 18.0f, 16.0f, 8.0f);
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  return ComputeOverlayListLayout(overlay).visible_rows;
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      return context_.current_project_state.overlay.workflow.compare_picker.matches.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.matches.size();
    case OverlayMode::ProjectSearch:
      return context_.current_project_state.overlay.workflow.project_search.results.size();
    case OverlayMode::Completion:
      return context_.current_project_state.overlay.workflow.completion.items.size();
    case OverlayMode::CodeActions:
      return context_.current_project_state.overlay.workflow.code_actions.items.size();
    case OverlayMode::FileFinder:
    default:
      return context_.current_project_state.file_finder.results().size();
  }
}

std::size_t WorkspaceShell::OverlaySelectedIndex() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      return context_.current_project_state.overlay.workflow.compare_picker.selected_index;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.selected_index;
    case OverlayMode::ProjectSearch:
      return context_.current_project_state.overlay.workflow.project_search.selected_index;
    case OverlayMode::Completion:
      return context_.current_project_state.overlay.workflow.completion.selected_index;
    case OverlayMode::CodeActions:
      return context_.current_project_state.overlay.workflow.code_actions.selected_index;
    case OverlayMode::FileFinder:
    default:
      return context_.current_project_state.file_finder.selected_index();
  }
}

void WorkspaceShell::SetOverlaySelectedIndex(std::size_t index) {
  const std::size_t item_count = OverlayItemCount();
  if (item_count == 0) {
    return;
  }
  const std::size_t clamped_index = std::min(index, item_count - 1);
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      context_.current_project_state.overlay.workflow.compare_picker.selected_index = clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      context_.current_project_state.overlay.workflow.buffer_search.selected_index = clamped_index;
      if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
        const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(match.start.line, match.start.column);
        }
      }
      break;
    case OverlayMode::ProjectSearch:
      context_.current_project_state.overlay.workflow.project_search.selected_index = clamped_index;
      break;
    case OverlayMode::Completion:
      context_.current_project_state.overlay.workflow.completion.selected_index = clamped_index;
      break;
    case OverlayMode::CodeActions:
      context_.current_project_state.overlay.workflow.code_actions.selected_index = clamped_index;
      break;
    case OverlayMode::FileFinder:
    default: {
      const std::size_t current_index = context_.current_project_state.file_finder.selected_index();
      context_.current_project_state.file_finder.MoveSelection(static_cast<int>(clamped_index) - static_cast<int>(current_index));
      break;
    }
  }
  RequestOverlayRedraw();
  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
      context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::ClampOverlayScrollRow(const SDL_FRect& overlay) {
  context_.current_project_state.overlay.scroll_row = ComputeOverlayListLayout(overlay).scroll_row;
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const auto layout = ComputeOverlayListLayout(overlay);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  context_.current_project_state.overlay.scroll_row = RevealScrollableListIndex(layout, selected);
  RequestOverlayRedraw();
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      return true;
    case OverlayMode::BufferSearch:
      if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
        const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
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
      if (!context_.current_project_state.overlay.workflow.project_search.results.empty() &&
          context_.current_project_state.overlay.workflow.project_search.selected_index <
              context_.current_project_state.overlay.workflow.project_search.results.size()) {
        const auto& result =
            context_.current_project_state.overlay.workflow.project_search.results[context_.current_project_state.overlay.workflow.project_search.selected_index];
        OpenFile(context_.current_project_state.root / result.relative_path);
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(result.line, result.column);
        }
        DismissOverlay(true);
      }
      return true;
    case OverlayMode::Completion:
      return ApplySelectedCompletion();
    case OverlayMode::CodeActions:
      return ExecuteSelectedCodeAction();
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = context_.current_project_state.file_finder.SelectedPath(); selected.has_value()) {
        OpenFile(context_.current_project_state.root / *selected);
      }
      DismissOverlay(true);
      return true;
  }
}

}  // namespace microide::workspace
