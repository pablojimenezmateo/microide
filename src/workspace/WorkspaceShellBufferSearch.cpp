#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

void WorkspaceShell::RefreshBufferSearch() {
  editor::TextViewport* viewport = ActiveEditorViewport();
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (viewport == nullptr) {
    buffer_search.matches.clear();
    buffer_search.selected_index = 0;
    return;
  }
  buffer_search.matches =
      FindLiteralSearchMatches(viewport->lines(), buffer_search.query.text());
  buffer_search.selected_index = 0;

  if (!buffer_search.matches.empty()) {
    RevealBufferSearchMatch(buffer_search.matches.front());
  }
  ResetOverlayScroll();
  RequestOverlayRedraw();
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::OpenBufferSearchFromProjectSearchResult() {
  auto& project_search = context_.current_project_state.overlay.workflow.project_search;
  if (project_search.results.empty() ||
      project_search.selected_index >= project_search.results.size()) {
    return;
  }
  std::string query = project_search.query.text();
  if (query.empty()) {
    return;
  }
  const auto& result = project_search.results[project_search.selected_index];
  const std::size_t target_line = result.line;
  const std::size_t target_column = result.column;

  // Carry the project-search term into the in-file find surface so the user can
  // keep moving between matches in the file they just opened.
  ShowOverlay(OverlayMode::BufferSearch);
  context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
  ResetBufferSearchFoldRevealState(false);

  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  buffer_search.query.SetText(std::move(query));
  buffer_search.replace_text.SetText("");
  RefreshBufferSearch();

  if (buffer_search.matches.empty()) {
    return;
  }

  // Start navigation on the match at (or after) the project-search hit rather
  // than the top of the file.
  std::size_t selected = buffer_search.matches.size() - 1;
  for (std::size_t i = 0; i < buffer_search.matches.size(); ++i) {
    const auto& start = buffer_search.matches[i].start;
    if (start.line > target_line ||
        (start.line == target_line && start.column >= target_column)) {
      selected = i;
      break;
    }
  }
  buffer_search.selected_index = selected;
  RevealBufferSearchMatch(buffer_search.matches[selected]);
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(buffer_search.selected_index);
  const int max_index = static_cast<int>(buffer_search.matches.size()) - 1;
  buffer_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  if (context_.current_project_state.overlay.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.matches.empty() ||
      buffer_search.selected_index >= buffer_search.matches.size()) {
    return;
  }

  buffer_search.preserve_temporarily_expanded_folds = true;
  const auto match = buffer_search.matches[buffer_search.selected_index];
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr ||
      !viewport->ReplaceRange(match, buffer_search.replace_text.text())) {
    return;
  }

  RefreshBufferSearch();
  if (!buffer_search.matches.empty()) {
    buffer_search.selected_index =
        std::min(buffer_search.selected_index, buffer_search.matches.size() - 1);
    RevealBufferSearchMatch(buffer_search.matches[buffer_search.selected_index]);
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  if (buffer_search.query.text().empty()) {
    return;
  }

  buffer_search.preserve_temporarily_expanded_folds = true;
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->ReplaceAll(buffer_search.query.text(), buffer_search.replace_text.text());
  }
  RefreshBufferSearch();
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RevealBufferSearchMatch(const editor::SelectionRange& match) {
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return;
  }
  viewport->MoveCursorTo(match.start.line, match.start.column);

  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
  if (model == nullptr) {
    return;
  }

  bool changed = false;
  for (std::size_t i = 0; i < model->ranges().size() && i < model->collapsed_flags().size(); ++i) {
    if (!model->collapsed_flags()[i]) {
      continue;
    }
    const auto& range = model->ranges()[i];
    if (match.start.line <= range.opener_line || match.start.line > range.closer_line) {
      continue;
    }
    if (model->Expand(range.opener_line)) {
      if (buffer_search.temporarily_expanded_fold_openers.empty()) {
        buffer_search.temporarily_expanded_fold_tab_path = viewport->path().lexically_normal();
      }
      if (std::find(buffer_search.temporarily_expanded_fold_openers.begin(),
                    buffer_search.temporarily_expanded_fold_openers.end(),
                    range.opener_line) ==
          buffer_search.temporarily_expanded_fold_openers.end()) {
        buffer_search.temporarily_expanded_fold_openers.push_back(range.opener_line);
      }
      changed = true;
    }
  }

  if (changed) {
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::ResetBufferSearchFoldRevealState(bool preserve_expanded_folds) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  const bool should_restore =
      !preserve_expanded_folds && !buffer_search.preserve_temporarily_expanded_folds &&
      !buffer_search.temporarily_expanded_fold_openers.empty();
  if (should_restore) {
    editor::TextViewport* viewport = ActiveEditorViewport();
    editor::FoldingModel* model = EnsureActiveFoldingModelFresh();
    if (viewport != nullptr && model != nullptr &&
        viewport->path().lexically_normal() == buffer_search.temporarily_expanded_fold_tab_path) {
      bool changed = false;
      for (auto it = buffer_search.temporarily_expanded_fold_openers.rbegin();
           it != buffer_search.temporarily_expanded_fold_openers.rend(); ++it) {
        changed = model->Collapse(*it) || changed;
      }
      if (changed) {
        RequestEditorSurfaceRedraw();
      }
    }
  }

  buffer_search.temporarily_expanded_fold_openers.clear();
  buffer_search.temporarily_expanded_fold_tab_path.clear();
  buffer_search.preserve_temporarily_expanded_folds = false;
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
