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

struct PlannedCaretEdit {
  SelectionRange removed;
  std::string replacement;
  std::optional<TextPosition> landed_override;
};

struct MultiCaretSite {
  TextPosition position;
  std::optional<SelectionRange> selection;
};

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

}

bool TextViewport::MultiCaretSelectionsOverlap() const {
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

CoalesceHint TextViewport::MultiCaretCoalesceHint(MultiCaretEditKind kind, std::string_view text,
                                                  bool per_caret) {
  switch (kind) {
    case MultiCaretEditKind::Insert: {
      const auto code_points = std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u;
      });
      if (per_caret || code_points != 1 || text == "\n" || text == "\t") {
        return {};
      }
      return CoalesceHint{.kind = CoalesceKind::Insert, .changed_is_space = text == " "};
    }
    case MultiCaretEditKind::Backspace:
      return CoalesceHint{.kind = CoalesceKind::DeleteBackward};
    case MultiCaretEditKind::DeleteForward:
      return CoalesceHint{.kind = CoalesceKind::DeleteForward};
    case MultiCaretEditKind::SoftTab:
    case MultiCaretEditKind::DeleteWordBackward:
    case MultiCaretEditKind::DeleteWordForward:
      break;
  }
  return {};
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
  const auto site_start = [](const MultiCaretSite& site) {
    return site.selection.has_value() ? site.selection->start : site.position;
  };
  const auto site_end = [](const MultiCaretSite& site) {
    return site.selection.has_value() ? site.selection->end : site.position;
  };
  std::sort(carets.begin(), carets.end(), [&](const MultiCaretSite& lhs, const MultiCaretSite& rhs) {
    const TextPosition lhs_start = site_start(lhs);
    const TextPosition rhs_start = site_start(rhs);
    if (detail::PositionLess(lhs_start, rhs_start)) {
      return true;
    }
    if (detail::PositionLess(rhs_start, lhs_start)) {
      return false;
    }
    return detail::PositionLess(site_end(lhs), site_end(rhs));
  });
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

  if (MultiCaretSelectionsOverlap()) {
    return false;
  }

  const auto clamp_position = [&](TextPosition position) -> TextPosition {
    position.line = std::min(position.line, document_->lines.size() - 1);
    position.column = TextLayout::ClampTextColumn(document_->lines[position.line], position.column);
    return position;
  };

  const std::size_t safe_indent_width = std::max<std::size_t>(1, indent_width_);
  const auto soft_tab_at = [&](std::size_t line, std::size_t column) {
    const std::size_t visual_column = VisualColumnAt(line, column);
    const std::size_t remainder = visual_column % safe_indent_width;
    return std::string(remainder == 0 ? safe_indent_width : safe_indent_width - remainder, ' ');
  };

  const auto plan_edit = [&](std::size_t line, std::size_t column,
                             const std::optional<SelectionRange>& selection,
                             std::string_view caret_insert)
      -> std::optional<PlannedCaretEdit> {
    if (selection.has_value()) {
      const SelectionRange removed{clamp_position(selection->start),
                                   clamp_position(selection->end)};
      if (kind == MultiCaretEditKind::SoftTab) {
        return PlannedCaretEdit{removed, soft_tab_at(removed.start.line, removed.start.column),
                                std::nullopt};
      }
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
      case MultiCaretEditKind::SoftTab:
        return PlannedCaretEdit{SelectionRange{TextPosition{line, column}, TextPosition{line, column}},
                                soft_tab_at(line, column), std::nullopt};
      case MultiCaretEditKind::Insert: {
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
              IndentStopBackspaceStart(line, column)
                  .value_or(TextLayout::PreviousTextColumn(document_->lines[line], column));
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
        const TextPosition target = WordTargetForCaret(caret, direction, true);
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

  detail::DisjointEditCapture capture(document_->lines);
  {
    std::vector<std::optional<detail::DisjointEditCapture::LineRange>> site_lines(planned.size());
    for (std::size_t i = 0; i < planned.size(); ++i) {
      if (planned[i].has_value()) {
        site_lines[i] = detail::DisjointEditCapture::LineRange{
            .first = planned[i]->removed.start.line, .last = planned[i]->removed.end.line};
      }
    }
    capture.PlanFootprints(site_lines);
  }

  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  const std::optional<SelectionRange> primary_selection =
      NormalizedSelection(primary_before, selection_anchor_);
  const std::size_t primary_index = static_cast<std::size_t>(
      std::find_if(carets.begin(), carets.end(),
                   [&](const MultiCaretSite& site) {
                     if (!(site.position == primary_before) ||
                         site.selection.has_value() != primary_selection.has_value()) {
                       return false;
                     }
                     return !site.selection.has_value() ||
                            (site.selection->start == primary_selection->start &&
                             site.selection->end == primary_selection->end);
                   }) -
      carets.begin());
  std::vector<detail::MultiCaretRemapSite> sites(carets.size());
  bool changed = false;
  bool collapsed_no_op = false;
  const std::uint64_t edit_base_revision = document_->content_revision;
  std::optional<SuppressEntryCaretCapture> suppress_dead_caret_copies;
  suppress_dead_caret_copies.emplace(*this);
  for (std::size_t i = carets.size(); i-- > 0;) {
    capture.EnterSite(i);
    const std::size_t line = std::min(carets[i].position.line, document_->lines.size() - 1);
    const std::size_t column =
        TextLayout::ClampTextColumn(document_->lines[line], carets[i].position.column);
    std::optional<HistoryEntry> entry =
        planned[i].has_value() ? BuildRangeHistoryEntry(planned[i]->removed, planned[i]->replacement)
                               : std::nullopt;
    if (!entry.has_value()) {
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
    capture.LeaveSite(i);
  }
  suppress_dead_caret_copies.reset();

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
  SetSecondaryCarets(std::move(updated_secondary_carets));
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  if (!changed) {
    EnsureCursorVisible();
    return false;
  }
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  HistoryEntry aggregate_entry = capture.Build(before_state, CaptureViewState());
  std::size_t edited_region_count = 0;
  for (const std::optional<PlannedCaretEdit>& plan : planned) {
    if (plan.has_value()) ++edited_region_count;
  }
  if (!aggregate_entry.is_multi_range() && edited_region_count <= 1) {
    SetLastAppliedEditFromEntry(aggregate_entry, true, edit_base_revision);
  } else {
    ClearLastAppliedEdit();
  }
  if (record_undo) {
    PushHistoryEntry(std::move(aggregate_entry),
                     MultiCaretCoalesceHint(kind, insert_text, per_caret_insert != nullptr));
  } else {
    document_->undo_history.ClearRedo();
  }
  return true;
}

namespace {

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

}

std::optional<std::string> TextViewport::MultiCaretSelectedText() const {
  if (secondary_carets_.empty()) {
    return std::nullopt;
  }
  const std::vector<MultiCaretSite> carets = CollectSortedCaretSites(
      TextPosition{cursor_line_, cursor_column_}, selection_anchor_, secondary_carets_);
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
  return ApplyMultiCaretEdit(MultiCaretEditKind::Backspace, "", record_undo);
}

bool TextViewport::ApplyMultiCaretSoftTab(bool record_undo) {
  return ApplyMultiCaretEdit(MultiCaretEditKind::SoftTab, "", record_undo);
}

bool TextViewport::PasteText(std::string_view text, bool record_undo) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (!has_multiple_carets()) {
    InsertText(text, record_undo);
    return true;
  }
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


bool TextViewport::AddCaretVertical(int delta) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty() || delta == 0) {
    return false;
  }
  document_->undo_history.NotifyCursorMoved();

  struct Seed {
    TextPosition position;
    std::size_t preferred_column;
    WrapRowAffinity affinity;
  };
  std::vector<Seed> seeds;
  seeds.reserve(secondary_carets_.size() + 1);
  seeds.push_back(Seed{TextPosition{cursor_line_, cursor_column_}, preferred_column_,
                       EffectiveCaretAffinity()});
  for (const SecondaryCaret& caret : secondary_carets_) {
    seeds.push_back(Seed{caret.position, caret.preferred_column, caret.wrap_affinity});
  }

  std::vector<Seed> added;
  added.reserve(seeds.size());
  for (const Seed& seed : seeds) {
    Seed next = seed;
    AdvanceCaretVertical(next.position, next.preferred_column, next.affinity, delta,
                         VerticalEdgePolicy::kClamp);
    if (!(next.position == seed.position)) {
      added.push_back(next);
    }
  }
  if (added.empty()) {
    return false;
  }

  const auto farther = [delta](const Seed& lhs, const Seed& rhs) {
    return delta > 0 ? detail::PositionLess(lhs.position, rhs.position)
                     : detail::PositionLess(rhs.position, lhs.position);
  };
  const Seed& lead = *std::max_element(added.begin(), added.end(), farther);

  std::vector<TextPosition> others;
  others.reserve(seeds.size() + added.size());
  for (const Seed& seed : seeds) {
    others.push_back(seed.position);
  }
  for (const Seed& seed : added) {
    if (!(seed.position == lead.position)) {
      others.push_back(seed.position);
    }
  }

  selection_anchor_.reset();
  PlacePrimaryCaret(lead.position.line, lead.position.column, false,
                    lead.affinity);
  preferred_column_ = lead.preferred_column;
  SetSecondaryCarets(std::move(others));
  EnsureCursorVisible();
  return true;
}

bool TextViewport::AddCaretsAtSelectedLineEnds() {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return false;
  }
  document_->undo_history.NotifyCursorMoved();

  std::size_t first = cursor_line_;
  std::size_t last = cursor_line_;
  if (const auto selection = selection_range(); selection.has_value()) {
    first = selection->start.line;
    last = selection->end.line;
    if (last > first && selection->end.column == 0) {
      --last;
    }
  }
  const std::size_t line_count = document_->lines.size();
  first = std::min(first, line_count - 1);
  last = std::min(last, line_count - 1);

  std::vector<TextPosition> ends;
  ends.reserve(last - first + 1);
  for (std::size_t line = first; line <= last; ++line) {
    ends.push_back(TextPosition{line, document_->lines.LineLength(line)});
  }
  if (ends.empty()) {
    return false;
  }

  selection_anchor_.reset();
  const TextPosition primary = ends.back();
  ends.pop_back();
  PlacePrimaryCaret(primary.line, primary.column);
  SetSecondaryCarets(std::move(ends));
  EnsureCursorVisible();
  return true;
}


bool TextViewport::AnySelectionContains(const TextPosition& position) const {
  const auto within = [&](const SelectionRange& range) {
    const SelectionRange normalized = NormalizeRange(range);
    return !detail::PositionLess(position, normalized.start) &&
           !detail::PositionLess(normalized.end, position);
  };
  if (const auto primary = selection_range(); primary.has_value() && within(*primary)) {
    return true;
  }
  for (const SecondaryCaret& caret : secondary_carets_) {
    if (const auto range = detail::SelectionRangeForSecondaryCaret(caret.position, caret.selection_anchor);
        range.has_value() && within(*range)) {
      return true;
    }
  }
  return false;
}

bool TextViewport::ExpandSelectionToWholeLines() {
  if (document_->lines.empty()) {
    return false;
  }
  document_->undo_history.NotifyCursorMoved();
  const std::size_t line_count = document_->lines.size();

  const auto whole_lines = [&](std::size_t first, std::size_t last) {
    first = std::min(first, line_count - 1);
    last = std::min(last, line_count - 1);
    return last + 1 < line_count
               ? SelectionRange{TextPosition{first, 0}, TextPosition{last + 1, 0}}
               : SelectionRange{TextPosition{first, 0},
                                TextPosition{last, document_->lines.LineLength(last)}};
  };
  const auto expand = [&](const std::optional<SelectionRange>& selection,
                          const TextPosition& caret) {
    if (!selection.has_value()) {
      return whole_lines(caret.line, caret.line);
    }
    const SelectionRange range = NormalizeRange(*selection);
    return whole_lines(range.start.line, range.end.line);
  };

  std::vector<SelectionRange> ranges;
  ranges.reserve(secondary_carets_.size() + 1);
  ranges.push_back(expand(selection_range(), TextPosition{cursor_line_, cursor_column_}));
  for (const SecondaryCaret& caret : secondary_carets_) {
    ranges.push_back(expand(
        detail::SelectionRangeForSecondaryCaret(caret.position, caret.selection_anchor),
        caret.position));
  }

  std::sort(ranges.begin(), ranges.end(), [](const SelectionRange& a, const SelectionRange& b) {
    return detail::PositionLess(a.start, b.start);
  });
  std::vector<SelectionRange> merged;
  for (const SelectionRange& range : ranges) {
    if (!merged.empty() && !detail::PositionLess(merged.back().end, range.start)) {
      if (detail::PositionLess(merged.back().end, range.end)) {
        merged.back().end = range.end;
      }
      continue;
    }
    merged.push_back(range);
  }

  const TextPosition previous{cursor_line_, cursor_column_};
  std::size_t primary_index = 0;
  for (std::size_t i = 0; i < merged.size(); ++i) {
    if (!detail::PositionLess(previous, merged[i].start) &&
        !detail::PositionLess(merged[i].end, previous)) {
      primary_index = i;
      break;
    }
  }
  const SelectionRange primary = merged[primary_index];
  merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(primary_index));

  MoveCursorTo(primary.start.line, primary.start.column, false);
  MoveCursorTo(primary.end.line, primary.end.column, true);
  SetSecondaryCaretsWithRanges(merged);
  EnsureCursorVisible();
  return true;
}

}
