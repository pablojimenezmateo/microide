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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// One caret's planned edit, computed from the pre-edit buffer. The reverse walk
// applies higher (later) carets first, and those edits never touch the content
// at a lower caret, so planning every edit up front from the original buffer is
// equivalent to recomputing it mid-walk.
struct PlannedCaretEdit {
  SelectionRange removed;
  std::string replacement;
  // Explicit landing position (brace-split insert); otherwise the caret lands
  // at the applied entry's after-state cursor.
  std::optional<TextPosition> landed_override;
};

// A caret plus the selection it owns (if any), so multi-caret Backspace / Delete /
// Enter / paste replace the selection like the single-caret paths do instead of
// dropping it and mis-editing one character per caret.
struct MultiCaretSite {
  TextPosition position;
  std::optional<SelectionRange> selection;
};

// Normalize a caret+anchor into an ordered selection range, or nullopt when there
// is no (non-empty) selection.
std::optional<SelectionRange> NormalizedSelection(const TextPosition& caret,
                                                  const std::optional<TextPosition>& anchor) {
  if (!anchor.has_value() || *anchor == caret) {
    return std::nullopt;
  }
  if (detail::PositionLess(*anchor, caret)) {
    return SelectionRange{*anchor, caret};
  }
  return SelectionRange{caret, *anchor};
}

}  // namespace

bool TextViewport::ApplyMultiCaretInsert(std::string_view text, bool record_undo) {
  return ApplyMultiCaretEdit(MultiCaretEditKind::Insert, text, record_undo);
}

bool TextViewport::ApplyMultiCaretBackspace(bool record_undo) {
  return ApplyMultiCaretEdit(MultiCaretEditKind::Backspace, "", record_undo);
}

bool TextViewport::ApplyMultiCaretDeleteForward(bool record_undo) {
  return ApplyMultiCaretEdit(MultiCaretEditKind::DeleteForward, "", record_undo);
}

bool TextViewport::ApplyMultiCaretEdit(MultiCaretEditKind kind, std::string_view insert_text,
                                       bool record_undo,
                                       const std::vector<std::string>* per_caret_insert) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
  }

  std::vector<MultiCaretSite> carets;
  carets.reserve(secondary_carets_.size() + 1);
  for (const SecondaryCaret& secondary : secondary_carets_) {
    carets.push_back(
        {secondary.position, NormalizedSelection(secondary.position, secondary.selection_anchor)});
  }
  const TextPosition primary_caret{cursor_line_, cursor_column_};
  carets.push_back({primary_caret, NormalizedSelection(primary_caret, selection_anchor_)});
  std::sort(carets.begin(), carets.end(), [](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
    return detail::PositionLess(lhs.position, rhs.position);
  });
  carets.erase(std::unique(carets.begin(), carets.end(),
                           [](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
                             return lhs.position == rhs.position;
                           }),
               carets.end());
  if (carets.empty()) {
    return false;
  }

  const auto clamp_position = [&](TextPosition position) -> TextPosition {
    position.line = std::min(position.line, document_->lines.size() - 1);
    position.column = TextLayout::ClampTextColumn(document_->lines[position.line], position.column);
    return position;
  };

  // Plan each caret's edit from the pre-edit buffer. Returns nullopt when the
  // caret cannot edit (backspace at doc start, delete at doc end).
  const auto plan_edit = [&](std::size_t line, std::size_t column,
                             const std::optional<SelectionRange>& selection,
                             std::string_view caret_insert)
      -> std::optional<PlannedCaretEdit> {
    // Selection-aware path: replace the selection (empty replacement for the
    // delete kinds, the inserted text otherwise), exactly as the single-caret
    // paths do. Without this, multi-caret Backspace/Delete/Enter/paste ignored
    // active selections and edited one character per caret instead.
    if (selection.has_value()) {
      const SelectionRange removed{clamp_position(selection->start),
                                   clamp_position(selection->end)};
      if (kind == MultiCaretEditKind::Insert) {
        if (caret_insert == "\n") {
          return PlannedCaretEdit{
              removed, "\n" + AutoIndentForNewline(removed.start.line, removed.start.column),
              std::nullopt};
        }
        return PlannedCaretEdit{removed, std::string(caret_insert), std::nullopt};
      }
      return PlannedCaretEdit{removed, "", std::nullopt};
    }
    switch (kind) {
      case MultiCaretEditKind::Insert: {
        // On Enter, a caret between a matching auto-close pair splits the braces
        // across three lines and lands on the inner-indent line (mirrors the
        // single-caret TryInsertNewlineSplitBraces path); other carets fall back
        // to a plain newline + auto-indent.
        if (caret_insert == "\n") {
          if (std::optional<NewlineBraceSplit> split = ComputeNewlineBraceSplit(line, column);
              split.has_value()) {
            return PlannedCaretEdit{
                SelectionRange{TextPosition{line, column}, TextPosition{line, column}},
                std::move(split->text),
                TextPosition{line + 1, split->inner_indent.size()}};
          }
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line, column}, TextPosition{line, column}},
              "\n" + AutoIndentForNewline(line, column), std::nullopt};
        }
        return PlannedCaretEdit{
            SelectionRange{TextPosition{line, column}, TextPosition{line, column}},
            std::string(caret_insert), std::nullopt};
      }
      case MultiCaretEditKind::Backspace: {
        if (column > 0) {
          const std::size_t erase_start =
              TextLayout::PreviousTextColumn(document_->lines[line], column);
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line, erase_start}, TextPosition{line, column}}, "",
              std::nullopt};
        }
        if (line > 0) {
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line - 1, document_->lines[line - 1].size()},
                             TextPosition{line, 0}},
              "", std::nullopt};
        }
        return std::nullopt;
      }
      case MultiCaretEditKind::DeleteForward: {
        if (column < document_->lines[line].size()) {
          const std::size_t erase_end =
              TextLayout::NextTextColumn(document_->lines[line], column);
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line, column}, TextPosition{line, erase_end}}, "",
              std::nullopt};
        }
        if (line + 1 < document_->lines.size()) {
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line, column}, TextPosition{line + 1, 0}}, "",
              std::nullopt};
        }
        return std::nullopt;
      }
    }
    return std::nullopt;
  };

  // Distribute one supplied string per caret (in sorted order) only when the
  // caller passed exactly one per deduped caret on an Insert; otherwise every
  // caret gets `insert_text`. The dedup above can shrink the set, so re-check
  // sizes here rather than trusting the pre-dedup caller count.
  const bool distribute = per_caret_insert != nullptr &&
                          kind == MultiCaretEditKind::Insert &&
                          per_caret_insert->size() == carets.size();

  std::vector<std::optional<PlannedCaretEdit>> planned;
  planned.reserve(carets.size());
  std::size_t affected_start = std::numeric_limits<std::size_t>::max();
  std::size_t affected_end = 0;
  bool has_candidate_edit = false;
  for (std::size_t caret_index = 0; caret_index < carets.size(); ++caret_index) {
    const MultiCaretSite& caret = carets[caret_index];
    const std::size_t line = std::min(caret.position.line, document_->lines.size() - 1);
    const std::size_t column =
        TextLayout::ClampTextColumn(document_->lines[line], caret.position.column);
    const std::string_view caret_insert =
        distribute ? std::string_view((*per_caret_insert)[caret_index]) : insert_text;
    std::optional<PlannedCaretEdit> edit = plan_edit(line, column, caret.selection, caret_insert);
    if (edit.has_value()) {
      affected_start = std::min(affected_start, edit->removed.start.line);
      affected_end = std::max(affected_end, edit->removed.end.line + 1);
      has_candidate_edit = true;
    }
    planned.push_back(std::move(edit));
  }
  if (!has_candidate_edit) {
    return false;
  }

  const std::size_t before_document_line_count = document_->lines.size();
  const LineSlice before_slice = CaptureLineSlice(document_->lines, affected_start, affected_end);
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  // Identify the primary by its index in the sorted/deduped vector rather than
  // by value-equality on the clamped position: a secondary caret can clamp onto
  // the primary's position, which would otherwise misattribute or drop a caret.
  const std::size_t primary_index = static_cast<std::size_t>(
      std::lower_bound(carets.begin(), carets.end(), primary_before,
                       [](const MultiCaretSite& site, const TextPosition& value) {
                         return detail::PositionLess(site.position, value);
                       }) -
      carets.begin());
  std::vector<ResultCaret> results;
  results.reserve(carets.size());
  bool changed = false;

  for (std::size_t i = carets.size(); i-- > 0;) {
    const bool is_primary = (i == primary_index);
    const std::size_t line = std::min(carets[i].position.line, document_->lines.size() - 1);
    const std::size_t column =
        TextLayout::ClampTextColumn(document_->lines[line], carets[i].position.column);
    std::optional<HistoryEntry> entry =
        planned[i].has_value() ? BuildRangeHistoryEntry(planned[i]->removed, planned[i]->replacement)
                               : std::nullopt;
    if (!entry.has_value()) {
      results.push_back(ResultCaret{TextPosition{line, column}, is_primary});
      continue;
    }
    changed = true;
    const SelectionRange removed = planned[i]->removed;
    const detail::ReplacementShape shape = detail::ComputeReplacementShape(planned[i]->replacement);
    ApplyHistoryEntry(*entry, true);
    for (ResultCaret& result : results) {
      result.position =
          detail::RemapPositionAfterReplace(result.position, removed.start, removed.end, shape);
    }
    const TextPosition landed = planned[i]->landed_override.value_or(
        TextPosition{entry->after_state.cursor_line, entry->after_state.cursor_column});
    results.push_back(ResultCaret{landed, is_primary});
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

namespace {

// Collect primary + secondary carets as sorted, position-deduped sites, each
// carrying its normalized selection. Shared by the multi-caret copy and the
// distribute-paste line split so both agree on caret order.
std::vector<MultiCaretSite> CollectSortedCaretSites(
    const TextPosition& primary, const std::optional<TextPosition>& primary_anchor,
    const std::vector<TextViewportUndoHistory::SecondaryCaret>& secondaries) {
  std::vector<MultiCaretSite> carets;
  carets.reserve(secondaries.size() + 1);
  for (const TextViewportUndoHistory::SecondaryCaret& secondary : secondaries) {
    carets.push_back(
        {secondary.position, NormalizedSelection(secondary.position, secondary.selection_anchor)});
  }
  carets.push_back({primary, NormalizedSelection(primary, primary_anchor)});
  std::sort(carets.begin(), carets.end(), [](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
    return detail::PositionLess(lhs.position, rhs.position);
  });
  carets.erase(std::unique(carets.begin(), carets.end(),
                           [](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
                             return lhs.position == rhs.position;
                           }),
               carets.end());
  return carets;
}

}  // namespace

std::optional<std::string> TextViewport::MultiCaretSelectedText() const {
  if (secondary_carets_.empty()) {
    return std::nullopt;  // single caret: caller uses SelectedText()
  }
  const std::vector<MultiCaretSite> carets = CollectSortedCaretSites(
      TextPosition{cursor_line_, cursor_column_}, selection_anchor_, secondary_carets_);
  // Only aggregate when every caret contributes a real selection (the Ctrl-D
  // case). Mixed selection/no-selection sets fall back to single-caret copy.
  for (const MultiCaretSite& caret : carets) {
    if (!caret.selection.has_value()) {
      return std::nullopt;
    }
  }
  std::string out;
  for (std::size_t i = 0; i < carets.size(); ++i) {
    if (i != 0) {
      out.push_back('\n');
    }
    out += TextInRange(*carets[i].selection);
  }
  return out;
}

bool TextViewport::DeleteMultiCaretSelections(bool record_undo) {
  // Each caret has a selection (caller guarantee), so the Backspace fan-out
  // replaces every selection with "" in one aggregate undo entry and never
  // touches a caret without a selection.
  return ApplyMultiCaretEdit(MultiCaretEditKind::Backspace, "", record_undo);
}

bool TextViewport::PasteText(std::string_view text, bool record_undo) {
  if (!has_multiple_carets()) {
    InsertText(text, record_undo);
    return true;
  }
  // Split the clipboard into lines (tolerating CRLF), then distribute one line
  // per caret only when the counts match; otherwise insert the whole payload at
  // every caret (ApplyMultiCaretEdit re-checks the count against the deduped set).
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      std::string_view line = text.substr(start, i - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      parts.emplace_back(line);
      start = i + 1;
    }
  }
  if (parts.size() == secondary_carets_.size() + 1) {
    return ApplyMultiCaretEdit(MultiCaretEditKind::Insert, text, record_undo, &parts);
  }
  InsertText(text, record_undo);
  return true;
}

}  // namespace microide::editor
