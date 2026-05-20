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
#include <limits>

#include "editor/TextLayout.h"

namespace microide::editor {

namespace {

struct LineSlice {
  std::size_t start = 0;
  std::vector<std::string> before_lines;
};

LineSlice CaptureLineSlice(const std::vector<std::string>& lines,
                           std::size_t start,
                           std::size_t end_exclusive) {
  const std::size_t clamped_start = std::min(start, lines.size());
  const std::size_t clamped_end = std::clamp(end_exclusive, clamped_start, lines.size());
  return LineSlice{
      .start = clamped_start,
      .before_lines =
          std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(clamped_start),
                                   lines.begin() + static_cast<std::ptrdiff_t>(clamped_end)),
  };
}

TextViewportUndoHistory::Entry BuildAggregateFromLineSlice(
    const LineSlice& slice,
    std::size_t before_document_line_count,
    const TextViewportUndoHistory::ViewState& before_state,
    const std::vector<std::string>& after_lines,
    const TextViewportUndoHistory::ViewState& after_state) {
  const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(after_lines.size()) -
                                    static_cast<std::ptrdiff_t>(before_document_line_count);
  const std::size_t after_slice_size =
      static_cast<std::size_t>(std::max<std::ptrdiff_t>(
          0, static_cast<std::ptrdiff_t>(slice.before_lines.size()) + line_delta));
  const std::size_t after_end =
      std::min(after_lines.size(), slice.start + after_slice_size);
  std::vector<std::string> after_slice(
      after_lines.begin() + static_cast<std::ptrdiff_t>(std::min(slice.start, after_lines.size())),
      after_lines.begin() + static_cast<std::ptrdiff_t>(after_end));

  TextViewportUndoHistory::Entry aggregate =
      TextViewportUndoHistory::BuildEntryForDocumentChange(slice.before_lines, before_state,
                                                           after_slice, after_state);
  aggregate.start_line += slice.start;
  return aggregate;
}

}  // namespace

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

  std::size_t affected_start = std::numeric_limits<std::size_t>::max();
  std::size_t affected_end = 0;
  for (const TextPosition& caret : carets) {
    const std::size_t line = std::min(caret.line, document_->lines.size() - 1);
    affected_start = std::min(affected_start, line);
    affected_end = std::max(affected_end, line + 1);
  }
  const std::size_t before_document_line_count = document_->lines.size();
  const LineSlice before_slice = CaptureLineSlice(document_->lines, affected_start, affected_end);
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

  const HistoryEntry aggregate_entry = BuildAggregateFromLineSlice(
      before_slice, before_document_line_count, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    undo_history_.ClearRedo();
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

  std::size_t affected_start = std::numeric_limits<std::size_t>::max();
  std::size_t affected_end = 0;
  bool has_candidate_edit = false;
  for (const TextPosition& caret : carets) {
    const std::size_t line = std::min(caret.line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], caret.column);
    if (column > 0) {
      affected_start = std::min(affected_start, line);
      affected_end = std::max(affected_end, line + 1);
      has_candidate_edit = true;
    } else if (line > 0) {
      affected_start = std::min(affected_start, line - 1);
      affected_end = std::max(affected_end, line + 1);
      has_candidate_edit = true;
    }
  }
  if (!has_candidate_edit) {
    return false;
  }
  const std::size_t before_document_line_count = document_->lines.size();
  const LineSlice before_slice = CaptureLineSlice(document_->lines, affected_start, affected_end);
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

  const HistoryEntry aggregate_entry = BuildAggregateFromLineSlice(
      before_slice, before_document_line_count, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    undo_history_.ClearRedo();
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

  std::size_t affected_start = std::numeric_limits<std::size_t>::max();
  std::size_t affected_end = 0;
  bool has_candidate_edit = false;
  for (const TextPosition& caret : carets) {
    const std::size_t line = std::min(caret.line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], caret.column);
    if (column < document_->lines[line].size()) {
      affected_start = std::min(affected_start, line);
      affected_end = std::max(affected_end, line + 1);
      has_candidate_edit = true;
    } else if (line + 1 < document_->lines.size()) {
      affected_start = std::min(affected_start, line);
      affected_end = std::max(affected_end, line + 2);
      has_candidate_edit = true;
    }
  }
  if (!has_candidate_edit) {
    return false;
  }
  const std::size_t before_document_line_count = document_->lines.size();
  const LineSlice before_slice = CaptureLineSlice(document_->lines, affected_start, affected_end);
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

  const HistoryEntry aggregate_entry = BuildAggregateFromLineSlice(
      before_slice, before_document_line_count, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    undo_history_.ClearRedo();
  }
  return true;
}

}  // namespace microide::editor
