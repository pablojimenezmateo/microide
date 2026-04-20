#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

void WorkspaceShell::RefreshBufferSearch() {
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    context_.current_project_state.overlay.workflow.buffer_search.matches.clear();
    context_.current_project_state.overlay.workflow.buffer_search.selected_index = 0;
    return;
  }
  context_.current_project_state.overlay.workflow.buffer_search.matches =
      FindLiteralSearchMatches(viewport->lines(), context_.current_project_state.overlay.workflow.buffer_search.query);
  context_.current_project_state.overlay.workflow.buffer_search.selected_index = 0;

  if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
    const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches.front();
    viewport->MoveCursorTo(match.start.line, match.start.column);
  }
  ResetOverlayScroll();
  RequestOverlayRedraw();
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  if (context_.current_project_state.overlay.workflow.buffer_search.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.selected_index);
  const int max_index = static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.matches.size()) - 1;
  context_.current_project_state.overlay.workflow.buffer_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  const auto& match =
      context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->MoveCursorTo(match.start.line, match.start.column);
  }
  if (context_.current_project_state.overlay.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (context_.current_project_state.overlay.workflow.buffer_search.matches.empty() ||
      context_.current_project_state.overlay.workflow.buffer_search.selected_index >=
          context_.current_project_state.overlay.workflow.buffer_search.matches.size()) {
    return;
  }

  const auto match =
      context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr ||
      !viewport->ReplaceRange(match, context_.current_project_state.overlay.workflow.buffer_search.replace_text)) {
    return;
  }

  RefreshBufferSearch();
  if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
    context_.current_project_state.overlay.workflow.buffer_search.selected_index =
        std::min(context_.current_project_state.overlay.workflow.buffer_search.selected_index,
                 context_.current_project_state.overlay.workflow.buffer_search.matches.size() - 1);
    const auto& next_match =
        context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
    viewport->MoveCursorTo(next_match.start.line, next_match.start.column);
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (context_.current_project_state.overlay.workflow.buffer_search.query.empty()) {
    return;
  }

  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->ReplaceAll(context_.current_project_state.overlay.workflow.buffer_search.query,
                         context_.current_project_state.overlay.workflow.buffer_search.replace_text);
  }
  RefreshBufferSearch();
  RequestEditorSurfaceRedraw();
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!context_.current_project_state.overlay.visible ||
      (context_.current_project_state.overlay.mode != OverlayMode::BufferSearch &&
       context_.current_project_state.overlay.mode != OverlayMode::BufferReplace) ||
      context_.current_project_state.overlay.workflow.buffer_search.matches.empty() ||
      context_.current_project_state.overlay.workflow.buffer_search.selected_index >=
          context_.current_project_state.overlay.workflow.buffer_search.matches.size()) {
    return std::nullopt;
  }
  return context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
}

}  // namespace microide::workspace
