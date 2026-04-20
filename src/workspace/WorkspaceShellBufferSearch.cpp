#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

void WorkspaceShell::RefreshBufferSearch() {
  overlay_workflow_.buffer_search.matches =
      FindLiteralSearchMatches(text_viewport_.lines(), overlay_workflow_.buffer_search.query);
  overlay_workflow_.buffer_search.selected_index = 0;

  if (!overlay_workflow_.buffer_search.matches.empty()) {
    const auto& match = overlay_workflow_.buffer_search.matches.front();
    text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  }
  ResetOverlayScroll();
  RequestOverlayRedraw();
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  if (overlay_workflow_.buffer_search.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(overlay_workflow_.buffer_search.selected_index);
  const int max_index = static_cast<int>(overlay_workflow_.buffer_search.matches.size()) - 1;
  overlay_workflow_.buffer_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  const auto& match =
      overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
  text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  if (overlay_state_.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (overlay_workflow_.buffer_search.matches.empty() ||
      overlay_workflow_.buffer_search.selected_index >=
          overlay_workflow_.buffer_search.matches.size()) {
    return;
  }

  const auto match =
      overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
  if (!text_viewport_.ReplaceRange(match, overlay_workflow_.buffer_search.replace_text)) {
    return;
  }

  RefreshBufferSearch();
  if (!overlay_workflow_.buffer_search.matches.empty()) {
    overlay_workflow_.buffer_search.selected_index =
        std::min(overlay_workflow_.buffer_search.selected_index,
                 overlay_workflow_.buffer_search.matches.size() - 1);
    const auto& next_match =
        overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
    text_viewport_.MoveCursorTo(next_match.start.line, next_match.start.column);
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (overlay_workflow_.buffer_search.query.empty()) {
    return;
  }

  text_viewport_.ReplaceAll(overlay_workflow_.buffer_search.query,
                            overlay_workflow_.buffer_search.replace_text);
  RefreshBufferSearch();
  RequestEditorSurfaceRedraw();
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!overlay_state_.visible ||
      (overlay_state_.mode != OverlayMode::BufferSearch &&
       overlay_state_.mode != OverlayMode::BufferReplace) ||
      overlay_workflow_.buffer_search.matches.empty() ||
      overlay_workflow_.buffer_search.selected_index >=
          overlay_workflow_.buffer_search.matches.size()) {
    return std::nullopt;
  }
  return overlay_workflow_.buffer_search.matches[overlay_workflow_.buffer_search.selected_index];
}

}  // namespace microide::workspace
