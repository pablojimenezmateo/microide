#include "workspace/BufferFindScope.h"

#include <algorithm>
#include <optional>

namespace microide::workspace {

BufferSearchState::Scope FindScopeAt(const editor::TextViewport& viewport) {
  BufferSearchState::Scope scope;
  if (const auto selection = viewport.selection_range();
      selection.has_value() && !(selection->start == selection->end)) {
    scope.range = editor::TextViewport::NormalizeRange(*selection);
  } else {
    const std::size_t line = viewport.cursor_line();
    scope.range = editor::SelectionRange{{line, 0}, {line, viewport.lines().LineLength(line)}};
  }
  StampFindScope(scope, viewport);
  return scope;
}

void StampFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport) {
  const editor::TextBuffer& lines = viewport.lines();
  const std::size_t last = lines.LineCount() == 0 ? 0 : lines.LineCount() - 1;
  scope.range.end.line = std::min(scope.range.end.line, last);
  scope.range.end.column = std::min(scope.range.end.column, lines.LineLength(scope.range.end.line));
  if (editor::PositionBefore(scope.range.end, scope.range.start)) {
    scope.range.start = scope.range.end;
  }
  scope.document = viewport.document_identity();
  scope.content_revision = viewport.content_revision();
  scope.lines_after_end = last - scope.range.end.line;
  scope.columns_after_end = lines.LineLength(scope.range.end.line) - scope.range.end.column;
}

bool FollowFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport) {
  if (scope.document != viewport.document_identity()) {
    return false;
  }
  if (scope.content_revision == viewport.content_revision()) {
    return true;
  }
  if (const auto& edit = viewport.last_applied_edit(); edit.has_value()) {
    scope.range.start = editor::MapPositionThroughEdit(scope.range.start, *edit);
    scope.range.end = editor::MapPositionThroughEdit(scope.range.end, *edit);
  } else {
    const editor::TextBuffer& lines = viewport.lines();
    const std::size_t last = lines.LineCount() == 0 ? 0 : lines.LineCount() - 1;
    scope.range.end.line = last - std::min(scope.lines_after_end, last);
    const std::size_t length = lines.LineLength(scope.range.end.line);
    scope.range.end.column = length - std::min(scope.columns_after_end, length);
  }
  StampFindScope(scope, viewport);
  return true;
}

void FollowFindScopes(BufferSearchState& state, const editor::TextViewport& viewport) {
  for (std::optional<BufferSearchState::Scope>* scope : {&state.scope, &state.opened_over}) {
    if (scope->has_value() && !FollowFindScope(**scope, viewport)) {
      scope->reset();
    }
  }
}

}  // namespace microide::workspace
