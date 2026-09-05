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

bool TextViewport::MultiCaretSelectionsOverlap() const {
  // Collect every caret's affected range (its selection, or an empty point at its
  // position), sorted by start. Any two ranges that intersect — or two carets
  // sharing a start position with different anchors — cannot be applied by either
  // multi-caret path without double-editing shared content, so the caller must
  // refuse the edit. Touching endpoints (adjacent edits) are allowed. This is
  // unreachable through normal UI, but SetSecondaryCaretsWithRanges / plugins /
  // future multi-cursor commands can construct it.
  std::vector<SelectionRange> ranges;
  ranges.reserve(secondary_carets_.size() + 1);
  const TextPosition primary{cursor_line_, cursor_column_};
  ranges.push_back(selection_range().value_or(SelectionRange{primary, primary}));
  for (const SecondaryCaret& secondary : secondary_carets_) {
    const std::optional<SelectionRange> selection =
        detail::SelectionRangeForSecondaryCaret(secondary.position, secondary.selection_anchor);
    ranges.push_back(selection.value_or(SelectionRange{secondary.position, secondary.position}));
  }
  std::sort(ranges.begin(), ranges.end(), [](const SelectionRange& lhs, const SelectionRange& rhs) {
    if (detail::PositionLess(lhs.start, rhs.start)) {
      return true;
    }
    if (detail::PositionLess(rhs.start, lhs.start)) {
      return false;
    }
    return detail::PositionLess(lhs.end, rhs.end);
  });
  // Two identical ranges are ONE site -- ApplyMultiCaretEdit collapses them --
  // not an overlap. A save-time trim can clamp a secondary caret onto the
  // primary; treating that as a conflict refused every later edit.
  ranges.erase(std::unique(ranges.begin(), ranges.end(),
                           [](const SelectionRange& lhs, const SelectionRange& rhs) {
                             return lhs.start == rhs.start && lhs.end == rhs.end;
                           }),
               ranges.end());
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].start == ranges[i - 1].start ||
        detail::PositionLess(ranges[i].start, ranges[i - 1].end)) {
      return true;
    }
  }
  return false;
}

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
  ClearLastAppliedEdit();
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
  // Collapse fully-identical sites (same caret AND same selection). Distinct sites
  // that merely share a caret position — two carets with different anchors — are
  // intentionally kept so the overlap check below can reject them rather than
  // silently discarding one replacement.
  carets.erase(std::unique(carets.begin(), carets.end(),
                           [](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
                             if (!(lhs.position == rhs.position)) {
                               return false;
                             }
                             if (lhs.selection.has_value() != rhs.selection.has_value()) {
                               return false;
                             }
                             return !lhs.selection.has_value() ||
                                    (lhs.selection->start == rhs.selection->start &&
                                     lhs.selection->end == rhs.selection->end);
                           }),
               carets.end());
  if (carets.empty()) {
    return false;
  }

  // Refuse overlapping / ambiguous sites before mutating anything (shared with the
  // TryMultiCaretPairInsert fast path). The reverse-walk apply assumes each caret
  // edits a DISJOINT region; overlapping selections would double-edit shared content
  // and corrupt the buffer.
  if (MultiCaretSelectionsOverlap()) {
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
          // Same indent-stop rule as the single-caret Backspace.
          const std::size_t erase_start =
              IndentStopBackspaceStart(line, column)
                  .value_or(TextLayout::PreviousTextColumn(document_->lines[line], column));
          // Same pair rule as the single-caret Backspace: `(|)` loses both.
          const std::size_t erase_end =
              CaretSitsInsideAutoClosedPair(line, column) ? column + 1 : column;
          return PlannedCaretEdit{
              SelectionRange{TextPosition{line, erase_start}, TextPosition{line, erase_end}}, "",
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
      case MultiCaretEditKind::DeleteWordBackward:
      case MultiCaretEditKind::DeleteWordForward: {
        const int direction = kind == MultiCaretEditKind::DeleteWordBackward ? -1 : 1;
        const TextPosition caret{line, column};
        const TextPosition target = WordTargetForCaret(caret, direction, /*for_deletion=*/true);
        if (target.line == caret.line && target.column == caret.column) {
          return std::nullopt;
        }
        return PlannedCaretEdit{direction < 0 ? SelectionRange{target, caret}
                                              : SelectionRange{caret, target},
                                "", std::nullopt};
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
      has_candidate_edit = true;
    }
    planned.push_back(std::move(edit));
  }
  if (!has_candidate_edit) {
    return false;
  }

  // Footprints: maximal runs of planned edits whose line ranges overlap or touch.
  // Two carets on the same line share one footprint, which is what keeps each
  // footprint's capture window untouched until it opens. Carets are already sorted
  // ascending, so their ranges are too (TD-2026-08-06-157).
  struct Footprint {
    std::size_t start = 0;
    std::size_t end_exclusive = 0;
    std::size_t first_caret = 0;
    std::size_t last_caret = 0;
  };
  std::vector<Footprint> footprints;
  for (std::size_t i = 0; i < planned.size(); ++i) {
    if (!planned[i].has_value()) {
      continue;
    }
    const std::size_t range_start = planned[i]->removed.start.line;
    const std::size_t range_end = planned[i]->removed.end.line + 1;
    if (!footprints.empty() && range_start <= footprints.back().end_exclusive) {
      footprints.back().end_exclusive = std::max(footprints.back().end_exclusive, range_end);
      footprints.back().last_caret = i;
      continue;
    }
    footprints.push_back(Footprint{.start = range_start,
                                   .end_exclusive = range_end,
                                   .first_caret = i,
                                   .last_caret = i});
  }
  // caret index -> footprint index, so the descending apply walk below can open a
  // footprint at its highest caret and close it at its lowest.
  constexpr std::size_t kNoFootprint = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> footprint_opens_at(planned.size(), kNoFootprint);
  std::vector<std::size_t> footprint_closes_at(planned.size(), kNoFootprint);
  for (std::size_t f = 0; f < footprints.size(); ++f) {
    footprint_opens_at[footprints[f].last_caret] = f;
    footprint_closes_at[footprints[f].first_caret] = f;
  }
  detail::DisjointEditCapture capture(document_->lines);

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
  // Apply edits high-to-low (so each edit's coordinates stay valid against the
  // still-unedited lower buffer), recording each caret's landed position + its
  // edit footprint indexed by ascending caret order. The lower-edit remap is then
  // done in a single batched pass (detail::ResolveMultiCaretRemapSites) instead of
  // re-remapping every already-produced result after each apply (which was
  // O(carets^2) per typed character with thousands of carets).
  std::vector<detail::MultiCaretRemapSite> sites(carets.size());
  bool changed = false;
  bool collapsed_no_op = false;
  for (std::size_t i = carets.size(); i-- > 0;) {
    if (footprint_opens_at[i] != kNoFootprint) {
      const Footprint& f = footprints[footprint_opens_at[i]];
      capture.BeginFootprint(f.start, f.end_exclusive);
    }
    const std::size_t line = std::min(carets[i].position.line, document_->lines.size() - 1);
    const std::size_t column =
        TextLayout::ClampTextColumn(document_->lines[line], carets[i].position.column);
    std::optional<HistoryEntry> entry =
        planned[i].has_value() ? BuildRangeHistoryEntry(planned[i]->removed, planned[i]->replacement)
                               : std::nullopt;
    if (!entry.has_value()) {
      // No buffer change. A planned edit that came back empty is an exact no-op
      // (the replacement reproduces the selected text): the caret still lands
      // past its replacement, as the single-caret path does.
      TextPosition landed{line, column};
      if (planned[i].has_value()) {
        const detail::ReplacementShape shape =
            detail::ComputeReplacementShape(planned[i]->replacement);
        landed = TextPosition{planned[i]->removed.start.line + shape.inserted_newlines,
                              shape.inserted_newlines == 0
                                  ? planned[i]->removed.start.column + shape.last_segment_cols
                                  : shape.last_segment_cols};
        collapsed_no_op = true;
      }
      sites[i] = detail::MultiCaretRemapSite{.landed = landed};
    } else {
      changed = true;
      ApplyHistoryEntry(*entry, true);
      const TextPosition landed = planned[i]->landed_override.value_or(
          TextPosition{entry->after_state.cursor_line, entry->after_state.cursor_column});
      sites[i] = detail::MultiCaretRemapSite{
          .landed = landed,
          .has_edit = true,
          .removed = planned[i]->removed,
          .shape = detail::ComputeReplacementShape(planned[i]->replacement)};
    }
    if (footprint_closes_at[i] != kNoFootprint) {
      capture.EndFootprint();
    }
  }

  if (!changed && !collapsed_no_op) {
    return false;
  }

  detail::ResolveMultiCaretRemapSites(sites);

  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(sites.size());
  for (std::size_t i = 0; i < sites.size(); ++i) {
    if (i == primary_index) {
      primary_after = sites[i].landed;
    } else {
      updated_secondary_carets.push_back(sites[i].landed);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  // cursor_line_/cursor_column_ were just set to primary_after above, so
  // SetSecondaryCarets clamps, sorts, dedups, and drops the primary in one pass.
  SetSecondaryCarets(std::move(updated_secondary_carets));
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  if (!changed) {
    // Every site was an exact no-op: the selections collapsed and the carets
    // moved, but the buffer is untouched, so nothing to dirty or record.
    EnsureCursorVisible();
    return false;
  }
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  HistoryEntry aggregate_entry = capture.Build(before_state, CaptureViewState());
  // Only publish an AppliedEdit for a single edited region. A multi-region edit is
  // not one replaced span, and single-range marker consumers (BreakpointStore, LSP
  // diagnostic shifting) would drag markers on the preserved lines between carets
  // to the span's end; leaving it empty makes them take their resync fallback
  // (breakpoints stay put, diagnostics re-request) instead of mis-collapsing.
  // `is_multi_range()` is now the direct test — the entry itself says whether it
  // covers one region or several (TD-2026-08-06-157).
  std::size_t edited_region_count = 0;
  for (const std::optional<PlannedCaretEdit>& plan : planned) {
    if (plan.has_value()) ++edited_region_count;
  }
  if (!aggregate_entry.is_multi_range() && edited_region_count <= 1) {
    SetLastAppliedEditFromEntry(aggregate_entry, true);
  } else {
    ClearLastAppliedEdit();
  }
  if (record_undo) {
    PushHistoryEntry(std::move(aggregate_entry));
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

bool TextViewport::ApplyMultiCaretSoftTab(bool record_undo) {
  // Build the same sorted + position-deduped caret set ApplyMultiCaretEdit derives
  // internally so the per-caret space strings line up by index, then size each
  // caret's soft tab to its own next tab stop.
  std::vector<TextPosition> positions;
  positions.reserve(secondary_carets_.size() + 1);
  for (const SecondaryCaret& secondary : secondary_carets_) {
    positions.push_back(secondary.position);
  }
  positions.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(positions.begin(), positions.end(), detail::PositionLess);
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

  const std::size_t safe_indent_width = std::max<std::size_t>(1, indent_width_);
  std::vector<std::string> parts;
  parts.reserve(positions.size());
  for (const TextPosition& position : positions) {
    std::size_t visual_column = 0;
    if (!document_->lines.empty() && position.line < document_->lines.size()) {
      // LineView + VisualColumnAt, not operator[]: the compatibility accessor
      // materializes a second copy of the line into the per-revision cache, and
      // the conversion answers in O(1) on a plain-ASCII line.
      const std::size_t column =
          TextLayout::ClampTextColumn(document_->lines.LineView(position.line), position.column);
      visual_column = VisualColumnAt(position.line, column);
    }
    const std::size_t remainder = visual_column % safe_indent_width;
    const std::size_t spaces = remainder == 0 ? safe_indent_width : safe_indent_width - remainder;
    parts.emplace_back(std::max<std::size_t>(1, spaces), ' ');
  }
  return ApplyMultiCaretEdit(MultiCaretEditKind::Insert, "", record_undo, &parts);
}

bool TextViewport::PasteText(std::string_view text, bool record_undo) {
  if (!has_multiple_carets()) {
    InsertText(text, record_undo);
    return true;
  }
  // Split the clipboard into lines (tolerating CRLF), then distribute one line
  // per caret only when the counts match; otherwise insert the whole payload at
  // every caret (ApplyMultiCaretEdit re-checks the count against the deduped set).
  // One trailing line break does not count as an extra (empty) line, as in VS
  // Code's spread rule: three whole lines copied with their final newline
  // still spread over three carets.
  std::string_view lines = text;
  if (lines.ends_with('\n')) {
    lines.remove_suffix(1);
  }
  if (lines.ends_with('\r')) {
    lines.remove_suffix(1);
  }
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= lines.size(); ++i) {
    if (i == lines.size() || lines[i] == '\n') {
      std::string_view line = lines.substr(start, i - start);
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
