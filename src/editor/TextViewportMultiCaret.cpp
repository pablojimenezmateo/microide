// Multi-caret apply pipeline for TextViewport. The three Apply* methods all
// follow the same shape: collect carets, walk them in reverse, build a single
// history entry per caret, then assemble one aggregate history entry covering
// the whole document delta. Split out of TextViewport.cpp so the apply
// pipeline can be inspected without the rest of the editor core.
//
// These methods are still members of the `TextViewport` class — see
// editor/TextViewport.h for the declarations.

#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

#include "editor/TextLayout.h"

namespace microide::editor {

bool TextViewport::ApplyMultiCaretInsert(std::string_view text, bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), detail::PositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    const std::string replacement =
        text == "\n" ? "\n" + AutoIndentForNewline(line, column) : std::string(text);
    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(
        SelectionRange{TextPosition{line, column}, TextPosition{line, column}}, replacement);
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), detail::PositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyMultiCaretBackspace(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), detail::PositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column > 0) {
      const std::size_t erase_start =
          TextLayout::PreviousTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, erase_start}, TextPosition{line, column}}, "");
    } else if (line > 0) {
      entry = BuildRangeHistoryEntry(SelectionRange{
                                         TextPosition{line - 1, document_->lines[line - 1].size()},
                                         TextPosition{line, 0},
                                     },
                                     "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), detail::PositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyMultiCaretDeleteForward(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), detail::PositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column < document_->lines[line].size()) {
      const std::size_t erase_end =
          TextLayout::NextTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line, erase_end}}, "");
    } else if (line + 1 < document_->lines.size()) {
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line + 1, 0}}, "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), detail::PositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

}  // namespace microide::editor
