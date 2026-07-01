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
#include <string_view>

#include "editor/TextLayout.h"

namespace microide::editor {

namespace {

struct LineSlice {
  std::size_t start = 0;
  std::vector<std::string> before_lines;
};

// Capture only the affected line range straight from the piece tree -- never
// materialize the whole document. Mirrors the range-scoped SliceLines capture
// used by the single-caret / range edit paths (see TextViewport.cpp).
LineSlice CaptureLineSlice(const TextBuffer& buffer,
                           std::size_t start,
                           std::size_t end_exclusive) {
  const std::size_t clamped_start = std::min(start, buffer.size());
  const std::size_t clamped_end = std::clamp(end_exclusive, clamped_start, buffer.size());
  return LineSlice{
      .start = clamped_start,
      .before_lines = buffer.SliceLines(clamped_start, clamped_end),
  };
}

TextViewportUndoHistory::Entry BuildAggregateFromLineSlice(
    const LineSlice& slice,
    std::size_t before_document_line_count,
    const TextViewportUndoHistory::ViewState& before_state,
    const TextBuffer& after_buffer,
    const TextViewportUndoHistory::ViewState& after_state) {
  const std::size_t after_document_line_count = after_buffer.size();
  const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(after_document_line_count) -
                                    static_cast<std::ptrdiff_t>(before_document_line_count);
  const std::size_t after_slice_size =
      static_cast<std::size_t>(std::max<std::ptrdiff_t>(
          0, static_cast<std::ptrdiff_t>(slice.before_lines.size()) + line_delta));
  const std::size_t after_start = std::min(slice.start, after_document_line_count);
  const std::size_t after_end =
      std::min(after_document_line_count, slice.start + after_slice_size);
  // Slice only the affected range out of the tree; the whole document is never
  // copied here.
  std::vector<std::string> after_slice = after_buffer.SliceLines(after_start, after_end);

  TextViewportUndoHistory::Entry aggregate =
      TextViewportUndoHistory::BuildEntryForDocumentChange(slice.before_lines, before_state,
                                                           after_slice, after_state);
  aggregate.start_line += slice.start;
  return aggregate;
}

struct ResultCaret {
  TextPosition position;
  bool is_primary = false;
};

}  // namespace

bool TextViewport::ApplyMultiCaretInsert(std::string_view text, bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
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
  // Identify the primary by its index in the sorted/deduped vector rather than
  // by value-equality on the clamped position: a secondary caret can clamp onto
  // the primary's position, which would otherwise misattribute or drop a caret.
  const std::size_t primary_index = static_cast<std::size_t>(
      std::lower_bound(carets.begin(), carets.end(), primary_before, detail::PositionLess) -
      carets.begin());
  std::vector<ResultCaret> results;
  results.reserve(carets.size());

  for (std::size_t i = carets.size(); i-- > 0;) {
    const bool is_primary = (i == primary_index);
    const std::size_t line = std::min(carets[i].line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], carets[i].column);
    // On Enter, a caret sitting between a matching auto-close pair splits the
    // braces across three lines and lands on the inner-indent line, mirroring
    // the single-caret TryInsertNewlineSplitBraces path. Other carets fall back
    // to a plain newline + auto-indent. Both share the reverse-walk fan-out.
    std::string replacement;
    std::optional<TextPosition> brace_split_caret;
    if (text == "\n") {
      if (std::optional<NewlineBraceSplit> split = ComputeNewlineBraceSplit(line, column);
          split.has_value()) {
        replacement = std::move(split->text);
        brace_split_caret = TextPosition{line + 1, split->inner_indent.size()};
      } else {
        replacement = "\n" + AutoIndentForNewline(line, column);
      }
    } else {
      replacement = std::string(text);
    }
    const SelectionRange removed{TextPosition{line, column}, TextPosition{line, column}};
    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(removed, replacement);
    if (!entry.has_value()) {
      results.push_back(ResultCaret{TextPosition{line, column}, is_primary});
      continue;
    }
    ApplyHistoryEntry(*entry, true);
    for (ResultCaret& result : results) {
      result.position =
          detail::RemapPositionAfterReplace(result.position, removed.start, removed.end, replacement);
    }
    const TextPosition landed = brace_split_caret.value_or(
        TextPosition{entry->after_state.cursor_line, entry->after_state.cursor_column});
    results.push_back(ResultCaret{landed, is_primary});
  }

  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(results.size());
  for (const ResultCaret& result : results) {
    if (result.is_primary) {
      primary_after = result.position;
    } else {
      updated_secondary_carets.push_back(result.position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  // cursor_line_/cursor_column_ were just set to primary_after above, so
  // SetSecondaryCarets clamps, sorts, dedups, and drops the primary in one pass.
  SetSecondaryCarets(std::move(updated_secondary_carets));
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
    document_->lines.PushBackLine("");
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
  const std::size_t primary_index = static_cast<std::size_t>(
      std::lower_bound(carets.begin(), carets.end(), primary_before, detail::PositionLess) -
      carets.begin());
  std::vector<ResultCaret> results;
  results.reserve(carets.size());
  bool changed = false;

  for (std::size_t i = carets.size(); i-- > 0;) {
    const bool is_primary = (i == primary_index);
    const std::size_t line = std::min(carets[i].line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], carets[i].column);
    std::optional<SelectionRange> removed;
    if (column > 0) {
      const std::size_t erase_start =
          TextLayout::PreviousTextColumn(document_->lines[line], column);
      removed = SelectionRange{TextPosition{line, erase_start}, TextPosition{line, column}};
    } else if (line > 0) {
      removed = SelectionRange{
          TextPosition{line - 1, document_->lines[line - 1].size()},
          TextPosition{line, 0},
      };
    }
    std::optional<HistoryEntry> entry =
        removed.has_value() ? BuildRangeHistoryEntry(*removed, "") : std::nullopt;
    if (!entry.has_value()) {
      results.push_back(ResultCaret{TextPosition{line, column}, is_primary});
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    for (ResultCaret& result : results) {
      result.position =
          detail::RemapPositionAfterReplace(result.position, removed->start, removed->end, "");
    }
    results.push_back(ResultCaret{
        TextPosition{entry->after_state.cursor_line, entry->after_state.cursor_column}, is_primary});
  }

  if (!changed) {
    return false;
  }

  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(results.size());
  for (const ResultCaret& result : results) {
    if (result.is_primary) {
      primary_after = result.position;
    } else {
      updated_secondary_carets.push_back(result.position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  // cursor_line_/cursor_column_ were just set to primary_after above, so
  // SetSecondaryCarets clamps, sorts, dedups, and drops the primary in one pass.
  SetSecondaryCarets(std::move(updated_secondary_carets));
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
    document_->lines.PushBackLine("");
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
  const std::size_t primary_index = static_cast<std::size_t>(
      std::lower_bound(carets.begin(), carets.end(), primary_before, detail::PositionLess) -
      carets.begin());
  std::vector<ResultCaret> results;
  results.reserve(carets.size());
  bool changed = false;

  for (std::size_t i = carets.size(); i-- > 0;) {
    const bool is_primary = (i == primary_index);
    const std::size_t line = std::min(carets[i].line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], carets[i].column);
    std::optional<SelectionRange> removed;
    if (column < document_->lines[line].size()) {
      const std::size_t erase_end =
          TextLayout::NextTextColumn(document_->lines[line], column);
      removed = SelectionRange{TextPosition{line, column}, TextPosition{line, erase_end}};
    } else if (line + 1 < document_->lines.size()) {
      removed = SelectionRange{TextPosition{line, column}, TextPosition{line + 1, 0}};
    }
    std::optional<HistoryEntry> entry =
        removed.has_value() ? BuildRangeHistoryEntry(*removed, "") : std::nullopt;
    if (!entry.has_value()) {
      results.push_back(ResultCaret{TextPosition{line, column}, is_primary});
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    for (ResultCaret& result : results) {
      result.position =
          detail::RemapPositionAfterReplace(result.position, removed->start, removed->end, "");
    }
    results.push_back(ResultCaret{
        TextPosition{entry->after_state.cursor_line, entry->after_state.cursor_column}, is_primary});
  }

  if (!changed) {
    return false;
  }

  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(results.size());
  for (const ResultCaret& result : results) {
    if (result.is_primary) {
      primary_after = result.position;
    } else {
      updated_secondary_carets.push_back(result.position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  // cursor_line_/cursor_column_ were just set to primary_after above, so
  // SetSecondaryCarets clamps, sorts, dedups, and drops the primary in one pass.
  SetSecondaryCarets(std::move(updated_secondary_carets));
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
