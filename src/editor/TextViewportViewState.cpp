#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

#include <execinfo.h>

#include "editor/WordBoundary.h"
#include "util/PerformanceCounters.h"
#include "util/Log.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {
namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;

}

void TextViewport::SetViewportSize(std::size_t visible_lines, std::size_t visible_columns) {
  const std::size_t next_visible_lines = std::max<std::size_t>(1, visible_lines);
  const std::size_t next_visible_columns = std::max<std::size_t>(8, visible_columns);
  const bool wrap_width_changed = soft_wrap_ && visible_columns_ != next_visible_columns;
  const bool reanchor = wrap_width_changed && document_ != nullptr && scroll_line_ != 0;
  const std::size_t anchor_line = reanchor ? VisualRowLineIndex(scroll_line_) : 0;
  const bool size_changed =
      visible_lines_ != next_visible_lines || visible_columns_ != next_visible_columns;
  visible_lines_ = next_visible_lines;
  visible_columns_ = next_visible_columns;
  if (wrap_width_changed) {
    horizontal_scroll_ = 0;
    if (document_ != nullptr) {
      InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
      if (reanchor) {
        scroll_line_ = VisualRowForLine(anchor_line);
      }
    }
  }
  ClampScrollState();
  if (pending_center_line_ != kNoPendingCenter) {
    const std::size_t line = pending_center_line_;
    pending_center_line_ = kNoPendingCenter;
    if (size_changed && document_ != nullptr && line < document_->lines.size()) {
      const std::size_t visual_row = VisualRowForLine(line);
      const std::size_t half = visible_lines_ / 2;
      scroll_line_ = visual_row > half ? visual_row - half : 0;
      ClampScrollState();
    }
  }
}

void TextViewport::SetScrollLine(std::size_t scroll_line) {
  scroll_line_ = scroll_line;
  ClampScrollState();
}

void TextViewport::SetHorizontalScroll(std::size_t horizontal_scroll) {
  horizontal_scroll_ = horizontal_scroll;
  ClampScrollState();
}

void TextViewport::ApplyRestoredViewState(std::size_t cursor_line,
                                          std::size_t cursor_column,
                                          std::size_t scroll_line,
                                          std::size_t horizontal_scroll,
                                          const std::optional<SelectionRange>& selection) {
  if (selection.has_value()) {
    MoveCursorTo(selection->start.line, selection->start.column);
    MoveCursorTo(selection->end.line, selection->end.column, true);
  } else {
    MoveCursorTo(cursor_line, cursor_column);
  }
  SetScrollLine(scroll_line);
  SetHorizontalScroll(horizontal_scroll);
}

void TextViewport::ReloadPreservingViewState(std::string_view text) {
  const std::size_t visible_lines_before = visible_lines();
  const std::size_t visible_columns_before = visible_columns();
  const std::size_t cursor_line_before = cursor_line();
  const std::size_t cursor_column_before = cursor_column();
  const std::size_t scroll_line_before = scroll_line();
  const std::size_t horizontal_scroll_before = horizontal_scroll();
  const std::optional<SelectionRange> selection_before = selection_range();
  const std::filesystem::path path_before = path();
  const std::optional<LineEnding> line_ending_before = line_ending();
  const util::FileSignature disk_signature_before =
      document_ != nullptr ? document_->disk_signature : util::FileSignature{};

  LoadContent(text, path_before, line_ending_before);
  document_->disk_signature = disk_signature_before;
  SetViewportSize(visible_lines_before, visible_columns_before);
  ApplyRestoredViewState(cursor_line_before, cursor_column_before, scroll_line_before,
                         horizontal_scroll_before, selection_before);
}

void TextViewport::SetTabSize(std::size_t tab_size) {
  const std::size_t next_tab_size = std::clamp<std::size_t>(tab_size, 1, 16);
  if (tab_size_ == next_tab_size) {
    return;
  }
  tab_size_ = next_tab_size;
  layout_cache_.ParkLineWidthsForTabSizeChange();
  if (document_ != nullptr) {
    InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::SetIndentWidth(std::size_t indent_width) {
  const std::size_t next_indent_width = std::clamp<std::size_t>(indent_width, 1, 16);
  if (indent_width_ == next_indent_width) {
    return;
  }
  indent_width_ = next_indent_width;
}

void TextViewport::SetSoftTabs(bool soft_tabs) {
  if (soft_tabs_ == soft_tabs) {
    return;
  }
  soft_tabs_ = soft_tabs;
}

void TextViewport::SetSoftWrap(bool soft_wrap) {
  if (soft_wrap_ == soft_wrap) {
    return;
  }
  const bool reanchor = document_ != nullptr && scroll_line_ != 0;
  const std::size_t anchor_line = reanchor ? VisualRowLineIndex(scroll_line_) : 0;
  const bool caret_was_visible = reanchor && [&] {
    const std::size_t caret_row = CursorVisualRow();
    return caret_row >= scroll_line_ && caret_row < scroll_line_ + visible_lines_;
  }();
  soft_wrap_ = soft_wrap;
  caret_wrap_affinity_ = WrapRowAffinity::kNextRow;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.wrap_affinity = WrapRowAffinity::kNextRow;
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  if (document_ != nullptr) {
    InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
    if (reanchor) {
      scroll_line_ = VisualRowForLine(anchor_line);
    }
  }
  ClampScrollState();
  if (caret_was_visible) {
    EnsureCursorVisible();
  }
}

void TextViewport::RevealCaret() {
  EnsureCursorVisible();
}

void TextViewport::SetFoldingModel(const FoldingModel* folding_model) {
  if (folding_model_ == folding_model) {
    return;
  }
  folding_model_ = folding_model;
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorVertical(int delta, bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty() || delta == 0) {
    return;
  }

  document_->undo_history.NotifyCursorMoved();
  TextPosition primary{cursor_line_, cursor_column_};
  WrapRowAffinity primary_affinity = EffectiveCaretAffinity();
  if (!extend_selection) {
    const auto collapse_to_edge = [&](TextPosition& caret,
                                      const std::optional<TextPosition>& anchor,
                                      std::size_t& preferred_column,
                                      WrapRowAffinity& affinity) {
      const std::optional<SelectionRange> selection =
          detail::SelectionRangeForSecondaryCaret(caret, anchor);
      if (!selection.has_value()) {
        return;
      }
      const TextPosition edge = delta < 0 ? selection->start : selection->end;
      if (edge == caret) {
        return;
      }
      caret = edge;
      affinity = WrapRowAffinity::kNextRow;
      preferred_column = PreferredColumnForCaret(caret, affinity);
    };
    collapse_to_edge(primary, selection_anchor_, preferred_column_, primary_affinity);
    for (SecondaryCaret& caret : secondary_carets_) {
      collapse_to_edge(caret.position, caret.selection_anchor, caret.preferred_column,
                       caret.wrap_affinity);
    }
  }
  BeginSelectionIfNeeded(extend_selection);
  AdvanceCaretVertical(primary, preferred_column_, primary_affinity, delta,
                       VerticalEdgePolicy::kToRowEdge);
  PlacePrimaryCaret(primary.line, primary.column, true, primary_affinity);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretVertical(caret.position, caret.preferred_column, caret.wrap_affinity, delta,
                         VerticalEdgePolicy::kToRowEdge);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorHorizontal(int delta, bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty() || delta == 0) {
    return;
  }

  document_->undo_history.NotifyCursorMoved();

  if (!extend_selection) {
    const bool to_end = delta > 0;
    bool any_selection = has_selection();
    for (const SecondaryCaret& caret : secondary_carets_) {
      if (caret.selection_anchor.has_value()) {
        any_selection = true;
        break;
      }
    }
    if (any_selection) {
      TextPosition primary{cursor_line_, cursor_column_};
      if (const std::optional<SelectionRange> sel = selection_range(); sel.has_value()) {
        primary = to_end ? sel->end : sel->start;
      } else {
        AdvanceCaretHorizontal(primary, delta);
      }
      for (SecondaryCaret& caret : secondary_carets_) {
        if (const std::optional<SelectionRange> sel =
                detail::SelectionRangeForSecondaryCaret(caret.position, caret.selection_anchor);
            sel.has_value()) {
          caret.position = to_end ? sel->end : sel->start;
        } else {
          AdvanceCaretHorizontal(caret.position, delta);
        }
        caret.selection_anchor.reset();
        caret.preferred_column = PreferredColumnForCaret(caret.position);
        caret.wrap_affinity = WrapRowAffinity::kNextRow;
      }
      selection_anchor_.reset();
      PlacePrimaryCaret(primary.line, primary.column);
      DedupeSecondaryCaretsAgainstPrimary();
      EnsureCursorVisible();
      return;
    }
  }

  BeginSelectionIfNeeded(extend_selection);
  TextPosition primary{cursor_line_, cursor_column_};
  AdvanceCaretHorizontal(primary, delta);
  PlacePrimaryCaret(primary.line, primary.column);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretHorizontal(caret.position, delta);
    caret.preferred_column = PreferredColumnForCaret(caret.position);
    caret.wrap_affinity = WrapRowAffinity::kNextRow;
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

TextPosition TextViewport::WordTargetForCaret(const TextPosition& caret,
                                             int delta,
                                             bool for_deletion) const {
  if (document_->lines.empty()) {
    return caret;
  }
  const std::size_t line = std::min(caret.line, document_->lines.size() - 1);
  const std::size_t length = document_->lines.LineLength(line);
  const std::size_t column = std::min(caret.column, length);
  if (delta < 0) {
    if (column == 0) {
      const std::size_t previous = VisibleLineBefore(line);
      return previous == line ? TextPosition{line, 0}
                              : TextPosition{previous, document_->lines.LineLength(previous)};
    }
    const std::string_view text = document_->lines.LineView(line);
    return TextPosition{line, for_deletion ? DeleteWordBoundaryLeft(text, column)
                                           : WordBoundaryLeft(text, column)};
  }
  if (column >= length) {
    const std::size_t next = VisibleLineAfter(line);
    return next == line ? TextPosition{line, length} : TextPosition{next, 0};
  }
  const std::string_view text = document_->lines.LineView(line);
  return TextPosition{line, for_deletion ? DeleteWordBoundaryRight(text, column)
                                         : WordBoundaryRight(text, column)};
}

void TextViewport::MoveCursorWord(int delta, bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty() || delta == 0) {
    return;
  }

  document_->undo_history.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const TextPosition primary =
      WordTargetForCaret(TextPosition{cursor_line_, cursor_column_}, delta, false);
  PlacePrimaryCaret(primary.line, primary.column);

  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position = WordTargetForCaret(caret.position, delta, false);
    caret.preferred_column = PreferredColumnForCaret(caret.position);
    caret.wrap_affinity = WrapRowAffinity::kNextRow;
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

TextViewport::ViewLineBounds TextViewport::ViewLineBoundsForCaret(
    const TextPosition& caret,
    WrapRowAffinity affinity) const {
  const std::size_t line_length =
      caret.line < document_->lines.size() ? document_->lines.LineLength(caret.line) : 0;
  const ViewLineBounds whole_line{0, line_length, false};
  if (!soft_wrap_ || document_->lines.empty() || caret.line >= document_->lines.size()) {
    return whole_line;
  }
  EnsureWrappedRowLayouts();
  if (layout_cache_.wrapped_row_layouts_trivial() || WrappedRowCount() == 0) {
    return whole_line;
  }
  const std::size_t row = CursorVisualRowForCaret(caret, affinity);
  const WrappedRowLayout layout = WrappedRowAt(row);
  if (layout.line_index != caret.line || layout.visual_end <= layout.visual_start) {
    return whole_line;
  }
  const std::size_t first = TextColumnAtVisualColumn(caret.line, layout.visual_start);
  const std::size_t last =
      std::min(TextColumnAtVisualColumn(caret.line, layout.visual_end), line_length);
  return ViewLineBounds{first, std::max(first, last), last < line_length};
}

std::size_t TextViewport::FirstNonWhitespaceColumnInView(std::size_t line,
                                                         const ViewLineBounds& bounds) const {
  if (line >= document_->lines.size()) {
    return bounds.first;
  }
  const std::string_view text = document_->lines.LineView(line);
  const std::size_t limit = std::min(bounds.last, text.size());
  for (std::size_t column = std::min(bounds.first, limit); column < limit; ++column) {
    if (text[column] != ' ' && text[column] != '\t') {
      return column;
    }
  }
  return bounds.first;
}

void TextViewport::MoveCursorLineStart(bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  document_->undo_history.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const auto home_target = [&](const TextPosition& caret, WrapRowAffinity affinity) {
    const ViewLineBounds bounds = ViewLineBoundsForCaret(caret, affinity);
    const std::size_t indent_end = FirstNonWhitespaceColumnInView(caret.line, bounds);
    return caret.column == indent_end ? bounds.first : indent_end;
  };
  PlacePrimaryCaret(cursor_line_,
                    home_target(TextPosition{cursor_line_, cursor_column_},
                                EffectiveCaretAffinity()));
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position.column = home_target(caret.position, caret.wrap_affinity);
    caret.preferred_column = PreferredColumnForCaret(caret.position);
    caret.wrap_affinity = WrapRowAffinity::kNextRow;
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  document_->undo_history.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const auto end_target = [this](const TextPosition& caret, WrapRowAffinity affinity) {
    const ViewLineBounds bounds = ViewLineBoundsForCaret(caret, affinity);
    if (bounds.last_is_wrap_point && caret.column == bounds.last &&
        affinity == WrapRowAffinity::kPreviousRow) {
      return std::pair{document_->lines.LineLength(caret.line), WrapRowAffinity::kNextRow};
    }
    return std::pair{bounds.last, bounds.last_is_wrap_point ? WrapRowAffinity::kPreviousRow
                                                            : WrapRowAffinity::kNextRow};
  };
  const auto [primary_column, primary_affinity] =
      end_target(TextPosition{cursor_line_, cursor_column_}, EffectiveCaretAffinity());
  PlacePrimaryCaret(cursor_line_, primary_column, false, primary_affinity);
  for (SecondaryCaret& caret : secondary_carets_) {
    if (caret.position.line < document_->lines.size()) {
      const auto [column, affinity] = end_target(caret.position, caret.wrap_affinity);
      caret.position.column = column;
      caret.wrap_affinity = affinity;
    }
    caret.preferred_column = PreferredColumnForCaret(caret.position, caret.wrap_affinity);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }

  document_->undo_history.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t line_length = document_->lines.LineLength(clamped_line);
  const std::size_t clamped_column = TextLayout::ClampTextColumn(
      document_->lines.LineView(clamped_line), std::min(column, line_length));
  PlacePrimaryCaret(clamped_line, clamped_column);
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  MergeOverlappingCaretRanges();
  EnsureCursorVisible();
}

void TextViewport::JumpCursorTo(std::size_t line, std::size_t column, bool extend_selection) {
  ClearSecondaryCarets();
  MoveCursorTo(line, column, extend_selection);
}

void TextViewport::MoveCursorToVisualColumn(std::size_t line,
                                            std::size_t visual_column,
                                            bool extend_selection) {
  if (document_->lines.empty()) {
    return;
  }

  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t text_column = TextColumnAtVisualColumn(clamped_line, visual_column);
  MoveCursorTo(clamped_line, text_column, extend_selection);
}

void TextViewport::CenterLine(std::size_t line) {
  if (document_->lines.empty()) {
    return;
  }
  const std::size_t clamped = std::min(line, document_->lines.size() - 1);
  pending_center_line_ = clamped;
  const std::size_t visual_row = VisualRowForLine(clamped);
  const std::size_t half = visible_lines_ / 2;
  scroll_line_ = visual_row > half ? visual_row - half : 0;
  ClampScrollState();
}

void TextViewport::ScrollVertical(int delta) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(scroll_line_);
  const int visual_rows = VisualRowCount();
  const int max_index = std::max(0, visual_rows - static_cast<int>(visible_lines_));
  scroll_line_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

void TextViewport::Page(int direction, bool extend_selection) {
  if (direction == 0) {
    return;
  }
  const std::size_t step = visible_lines_ > 1 ? visible_lines_ - 1 : 1;
  MoveCursorVertical(static_cast<int>(step) * direction, extend_selection);
}

bool TextViewport::PlainAsciiThroughColumn(std::size_t line, std::size_t column) const {
  if (document_ == nullptr) {
    return false;
  }
  return layout_cache_.PlainAsciiThroughColumn(document_->lines, line, column,
                                               document_->content_revision);
}

LineLayoutFacts TextViewport::CachedLineFacts(std::size_t line) const {
  if (document_ == nullptr) {
    return LineLayoutFacts{};
  }
  return layout_cache_.LineFactsIfCurrent(document_->lines.size(), line, tab_size_,
                                          document_->content_revision);
}

std::size_t TextViewport::PlainAsciiPrefixEnd(std::size_t line, std::size_t probe) const {
  if (document_ == nullptr || line >= document_->lines.size()) {
    return 0;
  }
  return layout_cache_.PlainAsciiPrefixEnd(LineSpan(document_->lines), line,
                                           std::min(probe, document_->lines.LineLength(line)),
                                           document_->content_revision);
}

std::size_t TextViewport::cursor_visual_column() const {
  return VisualColumnAt(cursor_line_, cursor_column_);
}

std::size_t TextViewport::VisualColumnAt(std::size_t line, std::size_t column) const {
  if (document_->lines.empty() || line >= document_->lines.size()) {
    return 0;
  }
  const LineLayoutFacts facts = CachedLineFacts(line);
  if (facts.known ? facts.plain_ascii : PlainAsciiThroughColumn(line, column)) {
    return std::min(column, document_->lines.LineLength(line));
  }
  if (facts.known && facts.tab_indented) {
    const std::size_t line_length =
        facts.leading_tabs + (facts.visual_columns - facts.leading_tabs * tab_size_);
    const std::size_t clamped = std::min(column, line_length);
    return clamped <= facts.leading_tabs
               ? clamped * tab_size_
               : facts.leading_tabs * tab_size_ + (clamped - facts.leading_tabs);
  }
  const std::string_view text = document_->lines.LineView(line);
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes,
                              std::min(column, text.size()));
  util::AddPerformanceCounter(facts.known ? util::PerfCounterId::EditorVisualColumnWalkNotPlainAscii
                                          : util::PerfCounterId::EditorVisualColumnWalkFactsUnknown);
  return TextLayout::VisualColumnForTextColumn(text, column, tab_size_);
}

std::size_t TextViewport::TextColumnAtVisualColumn(std::size_t line,
                                                   std::size_t visual_column) const {
  if (document_->lines.empty() || line >= document_->lines.size()) {
    return 0;
  }
  const LineLayoutFacts facts = CachedLineFacts(line);
  if (facts.known ? facts.plain_ascii : PlainAsciiThroughColumn(line, visual_column)) {
    return std::min(visual_column, document_->lines.LineLength(line));
  }
  if (facts.known && facts.tab_indented) {
    const std::size_t line_length =
        facts.leading_tabs + (facts.visual_columns - facts.leading_tabs * tab_size_);
    const std::size_t indent_width = facts.leading_tabs * tab_size_;
    if (visual_column >= indent_width) {
      return std::min(facts.leading_tabs + (visual_column - indent_width), line_length);
    }
    const std::size_t into_tab = visual_column % tab_size_;
    return visual_column / tab_size_ + (into_tab * 2 <= tab_size_ ? 0 : 1);
  }
  const std::string_view text = document_->lines.LineView(line);
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes,
                              std::min(visual_column, text.size()));
  util::AddPerformanceCounter(facts.known ? util::PerfCounterId::EditorVisualColumnWalkNotPlainAscii
                                          : util::PerfCounterId::EditorVisualColumnWalkFactsUnknown);
  return TextLayout::TextColumnForVisualColumn(text, visual_column, tab_size_);
}

void TextViewport::ClampScrollState() {
  if (scroll_line_ != 0) {
    const std::size_t total_visual_lines = visual_line_count();
    const std::size_t max_vertical_scroll =
        total_visual_lines > visible_lines_ ? total_visual_lines - visible_lines_ : 0;
    scroll_line_ = std::min(scroll_line_, max_vertical_scroll);
  }

  if (soft_wrap_) {
    horizontal_scroll_ = 0;
    return;
  }
  if (horizontal_scroll_ == 0) {
    return;
  }
  const std::size_t max_visual_columns = MaxVisualColumns();
  const std::size_t max_horizontal_scroll =
      max_visual_columns > visible_columns_ ? max_visual_columns - visible_columns_ : 0;
  horizontal_scroll_ = std::min(horizontal_scroll_, max_horizontal_scroll);
}

void TextViewport::PlacePrimaryCaret(std::size_t line,
                                     std::size_t column,
                                     bool keep_preferred_column,
                                     WrapRowAffinity affinity) {
  cursor_line_ = line;
  cursor_column_ = column;
  caret_wrap_affinity_ = affinity;
  caret_wrap_affinity_position_ = TextPosition{line, column};
  if (!keep_preferred_column) {
    preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_}, affinity);
  }
  caret_navigation_content_revision_ =
      document_ != nullptr ? document_->content_revision : 0;
  column_selection_ = ColumnSelectionState{};
}

std::string TextViewport::CaretBoundsFault() const {
  if (document_ == nullptr) {
    return {};
  }
  const auto outside = [this](const TextPosition& p) {
    return p.line >= document_->lines.size() || p.column > document_->lines.LineLength(p.line);
  };
  const auto describe = [this](const char* what, const TextPosition& p) {
    const std::string length = p.line < document_->lines.size() ? std::to_string(document_->lines.LineLength(p.line))
                                                                : std::string("no such line");
    return std::string(what) + " at " + std::to_string(p.line + 1) + ":" + std::to_string(p.column + 1) +
           " (line length " + length + ", " + std::to_string(document_->lines.size()) + " lines)";
  };
  if (const TextPosition primary{cursor_line_, cursor_column_}; outside(primary)) {
    return describe("caret", primary);
  }
  if (selection_anchor_.has_value() && outside(*selection_anchor_)) {
    return describe("selection anchor", *selection_anchor_);
  }
  for (const SecondaryCaret& caret : secondary_carets_) {
    if (outside(caret.position)) {
      return describe("secondary caret", caret.position);
    }
    if (caret.selection_anchor.has_value() && outside(*caret.selection_anchor)) {
      return describe("secondary anchor", *caret.selection_anchor);
    }
  }
  return {};
}

void TextViewport::TraceCaretBounds(const char* where) const {
  static const bool enabled = util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_CARET_BOUNDS");
  if (!enabled || caret_bounds_reported_ || document_ == nullptr || CaretsLagDocument()) {
    return;
  }
  const std::string fault = CaretBoundsFault();
  if (fault.empty()) {
    return;
  }
  caret_bounds_reported_ = true;
  std::string report = "microide caret-bounds: " + std::string(where) + ": " + document_->path.generic_string() +
                       " has its " + fault + "; the code that left it:\n";
  void* frames[32];
  const int count = ::backtrace(frames, 32);
  if (char** symbols = ::backtrace_symbols(frames, count); symbols != nullptr) {
    for (int i = 1; i < count; ++i) {
      report += "  #" + std::to_string(i) + " " + symbols[i] + "\n";
    }
    std::free(symbols);  // NOLINT(cppcoreguidelines-no-malloc): backtrace_symbols' contract
  }
  util::Log(report);
}

void TextViewport::EnsureCursorVisible() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureCursorVisible");
  TraceCaretBounds("after a move or an edit");
  const std::size_t cursor_visual_row = CursorVisualRow();
  if (cursor_visual_row < scroll_line_) {
    scroll_line_ = cursor_visual_row;
  }

  const std::size_t vertical_margin =
      std::min(kScrollMargin, visible_lines_ > 0 ? visible_lines_ - 1 : 0);
  if (cursor_visual_row < scroll_line_ + vertical_margin) {
    scroll_line_ = cursor_visual_row > vertical_margin ? cursor_visual_row - vertical_margin : 0;
  } else {
    const std::size_t visible_span =
        visible_lines_ > vertical_margin ? visible_lines_ - vertical_margin - 1 : 0;
    if (cursor_visual_row > scroll_line_ + visible_span) {
      scroll_line_ = cursor_visual_row > visible_span ? cursor_visual_row - visible_span : 0;
    }
  }

  if (soft_wrap_) {
    ClampScrollState();
    return;
  }

  const std::size_t visual_cursor_column = cursor_visual_column();
  if (visual_cursor_column < horizontal_scroll_ + kHorizontalScrollMargin) {
    horizontal_scroll_ = visual_cursor_column > kHorizontalScrollMargin
                             ? visual_cursor_column - kHorizontalScrollMargin
                             : 0;
  } else {
    const std::size_t visible_width = visible_columns_ > 0 ? visible_columns_ : 1;
    const std::size_t horizontal_span = visible_width > kHorizontalScrollMargin
                                            ? visible_width - kHorizontalScrollMargin - 1
                                            : 0;
    if (visual_cursor_column > horizontal_scroll_ + horizontal_span) {
      horizontal_scroll_ =
          visual_cursor_column > horizontal_span ? visual_cursor_column - horizontal_span : 0;
    }
  }

  ClampScrollState();
}

std::size_t TextViewport::CursorVisualRow() const {
  return CursorVisualRowForCaret(TextPosition{cursor_line_, cursor_column_},
                                 EffectiveCaretAffinity());
}

std::size_t TextViewport::cursor_visual_row() const {
  return CursorVisualRow();
}

std::size_t TextViewport::PreferredColumnForCaret(const TextPosition& caret,
                                                  WrapRowAffinity affinity) const {
  if (caret.line >= document_->lines.size()) {
    return 0;
  }
  const std::size_t visual = VisualColumnAt(caret.line, caret.column);
  if (!soft_wrap_) {
    return visual;
  }
  EnsureWrappedRowLayouts();
  if (WrappedRowCount() == 0) {
    return 0;
  }
  const WrappedRowLayout row = WrappedRowAt(CursorVisualRowForCaret(caret, affinity));
  return row.indent + (visual >= row.visual_start ? visual - row.visual_start : 0);
}

std::size_t TextViewport::CursorVisualRowForCaret(const TextPosition& caret,
                                                  WrapRowAffinity affinity) const {
  EnsureWrappedRowLayouts();
  if (document_->lines.empty() || caret.line >= document_->lines.size()) {
    return 0;
  }
  if (layout_cache_.wrapped_row_layouts_trivial()) {
    return caret.line;
  }
  if (!layout_cache_.has_wrapped_line_row_offsets(document_->lines.size())) {
    return 0;
  }
  const std::size_t base_row = layout_cache_.WrappedLineRowOffset(caret.line);
  if (folding_model_ != nullptr && folding_model_->IsLineHidden(caret.line)) {
    if (!soft_wrap_) {
      return base_row;
    }
    const std::size_t opener_line = WrappedRowAt(base_row).line_index;
    return layout_cache_.WrappedRowRangeForLine(opener_line, document_->lines.size()).second;
  }
  if (!soft_wrap_) {
    return base_row;
  }
  const std::size_t caret_visual = VisualColumnAt(caret.line, caret.column);
  const auto [first_row, last_row] =
      layout_cache_.WrappedRowRangeForLine(caret.line, document_->lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorWrapCaretRowResolves);
  return layout_cache_.WrappedRowForVisualColumn(first_row, last_row, caret_visual,
                                                 affinity == WrapRowAffinity::kPreviousRow);
}

std::size_t TextViewport::ResolveSoftWrapCursorColumnForTargetRow(std::size_t preferred_column,
                                                                  std::size_t target_row) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return 0;
  }
  const std::size_t clamped_row = std::min(target_row, row_count - 1);
  const WrappedRowLayout target = WrappedRowAt(clamped_row);
  if (!soft_wrap_) {
    return preferred_column;
  }
  if (target.visual_end <= target.visual_start) {
    return target.visual_start;
  }
  const std::size_t content_offset =
      preferred_column > target.indent ? preferred_column - target.indent : 0;
  return std::min(target.visual_start + content_offset, target.visual_end);
}

WrapRowAffinity TextViewport::AffinityForRowLanding(std::size_t row_index,
                                                    std::size_t visual_column) const {
  if (!soft_wrap_) {
    return WrapRowAffinity::kNextRow;
  }
  const WrappedRowLayout row = WrappedRowAt(row_index);
  if (visual_column != row.visual_end) {
    return WrapRowAffinity::kNextRow;
  }
  if (row_index + 1 >= WrappedRowCount() ||
      WrappedRowAt(row_index + 1).line_index != row.line_index) {
    return WrapRowAffinity::kNextRow;
  }
  return WrapRowAffinity::kPreviousRow;
}

void TextViewport::AdvanceCaretHorizontal(TextPosition& caret, int delta) const {
  if (document_->lines.empty()) {
    return;
  }
  if (caret.line >= document_->lines.size()) {
    caret.line = document_->lines.size() - 1;
  }
  caret.column = std::min(caret.column, document_->lines.LineLength(caret.line));
  if (delta < 0) {
    for (int i = delta; i < 0; ++i) {
      if (caret.column == 0) {
        const std::size_t previous = VisibleLineBefore(caret.line);
        if (previous == caret.line) {
          break;
        }
        caret.line = previous;
        caret.column = document_->lines.LineLength(caret.line);
        continue;
      }
      caret.column = TextLayout::PreviousTextColumn(document_->lines.LineView(caret.line), caret.column);
    }
  } else {
    for (int i = 0; i < delta; ++i) {
      const std::string_view line = document_->lines.LineView(caret.line);
      if (caret.column >= line.size()) {
        const std::size_t next = VisibleLineAfter(caret.line);
        if (next == caret.line) {
          break;
        }
        caret.line = next;
        caret.column = 0;
        continue;
      }
      caret.column = TextLayout::NextTextColumn(line, caret.column);
    }
  }
}

TextPosition TextViewport::ClampedToDocument(TextPosition position) const {
  if (document_->lines.empty()) {
    return TextPosition{0, 0};
  }
  position.line = std::min(position.line, document_->lines.size() - 1);
  position.column = ReadCaretNeighborhood(position.line, position.column).clamped_column;
  return position;
}

void TextViewport::ClampCaretSetToDocument() {
  const auto clamp_position = [this](TextPosition& position) { position = ClampedToDocument(position); };
  TextPosition primary{cursor_line_, cursor_column_};
  clamp_position(primary);
  cursor_line_ = primary.line;
  cursor_column_ = primary.column;
  if (selection_anchor_.has_value()) {
    clamp_position(*selection_anchor_);
  }
  for (SecondaryCaret& caret : secondary_carets_) {
    clamp_position(caret.position);
    if (caret.selection_anchor.has_value()) {
      clamp_position(*caret.selection_anchor);
    }
  }
}

void TextViewport::SyncCaretsWithSharedDocument() {
  carets_synced_content_revision_ = document_->content_revision;
  ClampCaretSetToDocument();
  DedupeSecondaryCaretsAgainstPrimary();
  ClampScrollState();
}

void TextViewport::DedupeSecondaryCaretsAgainstPrimary() {
  detail::SortSecondaryCaretsIfNeeded(secondary_carets_);
  secondary_carets_.erase(
      std::unique(secondary_carets_.begin(), secondary_carets_.end(),
                  [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
                    return lhs.position == rhs.position &&
                           lhs.selection_anchor == rhs.selection_anchor;
                  }),
      secondary_carets_.end());
  const TextPosition primary{cursor_line_, cursor_column_};
  secondary_carets_.erase(
      std::remove_if(secondary_carets_.begin(), secondary_carets_.end(),
                     [&](const SecondaryCaret& caret) {
                       return caret.position == primary &&
                              caret.selection_anchor == selection_anchor_;
                     }),
      secondary_carets_.end());
  MergeOverlappingCaretRanges();
}

void TextViewport::MergeOverlappingCaretRanges() {
  bool any_selection = selection_anchor_.has_value();
  for (const SecondaryCaret& caret : secondary_carets_) {
    any_selection = any_selection || caret.selection_anchor.has_value();
  }
  if (!any_selection || secondary_carets_.empty()) {
    return;
  }

  constexpr std::size_t kPrimary = std::numeric_limits<std::size_t>::max();
  using Entry = CaretMergeEntry;
  std::vector<Entry>& entries = caret_merge_scratch_;
  entries.clear();
  const std::size_t needed = secondary_carets_.size() + 1;
  if (entries.capacity() < needed) {
    entries.reserve(std::max(needed, entries.capacity() * 2));
  }
  const auto push = [&](const TextPosition& caret, const std::optional<TextPosition>& anchor,
                        std::size_t source) {
    if (!anchor.has_value() || *anchor == caret) {
      entries.push_back(Entry{caret, caret, true, source});
    } else if (detail::PositionLess(*anchor, caret)) {
      entries.push_back(Entry{*anchor, caret, true, source});
    } else {
      entries.push_back(Entry{caret, *anchor, false, source});
    }
  };
  push(TextPosition{cursor_line_, cursor_column_}, selection_anchor_, kPrimary);
  for (std::size_t i = 0; i < secondary_carets_.size(); ++i) {
    push(secondary_carets_[i].position, secondary_carets_[i].selection_anchor, i);
  }
  const auto by_start = [](const Entry& lhs, const Entry& rhs) {
    if (detail::PositionLess(lhs.start, rhs.start)) return true;
    if (detail::PositionLess(rhs.start, lhs.start)) return false;
    return detail::PositionLess(lhs.end, rhs.end);
  };
  if (!std::is_sorted(entries.begin(), entries.end(), by_start)) {
    std::sort(entries.begin(), entries.end(), by_start);
  }

  bool merged_any = false;
  std::size_t out = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Entry& entry = entries[i];
    if (out > 0) {
      Entry& prev = entries[out - 1];
      const bool prev_collapsed = prev.start == prev.end;
      const bool collapsed = entry.start == entry.end;
      const bool merge = (prev_collapsed || collapsed)
                             ? !detail::PositionLess(prev.end, entry.start)
                             : detail::PositionLess(entry.start, prev.end);
      if (merge) {
        merged_any = true;
        if (detail::PositionLess(prev.end, entry.end)) {
          prev.end = entry.end;
        }
        if (entry.source == kPrimary) {
          prev.source = kPrimary;
          prev.caret_at_end = entry.caret_at_end;
        }
        continue;
      }
    }
    entries[out++] = entry;
  }
  if (!merged_any) {
    return;
  }
  entries.resize(out);

  std::size_t written = 0;
  for (const Entry& entry : entries) {
    const bool collapsed = entry.start == entry.end;
    const TextPosition caret = entry.caret_at_end ? entry.end : entry.start;
    const std::optional<TextPosition> anchor =
        collapsed ? std::nullopt
                  : std::optional<TextPosition>(entry.caret_at_end ? entry.start : entry.end);
    if (entry.source == kPrimary) {
      cursor_line_ = caret.line;
      cursor_column_ = caret.column;
      selection_anchor_ = anchor;
      caret_wrap_affinity_ = WrapRowAffinity::kNextRow;
      preferred_column_ = PreferredColumnForCaret(caret);
      continue;
    }
    secondary_carets_[written++] = SecondaryCaret{
        .position = caret,
        .preferred_column = PreferredColumnForCaret(caret),
        .selection_anchor = anchor,
    };
  }
  secondary_carets_.resize(written);
  detail::SortSecondaryCaretsIfNeeded(secondary_carets_);
}

void TextViewport::AdvanceCaretVertical(TextPosition& caret,
                                        std::size_t& preferred_column,
                                        WrapRowAffinity& affinity,
                                        int delta,
                                        VerticalEdgePolicy edge_policy) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return;
  }
  const std::size_t current_row = CursorVisualRowForCaret(caret, affinity);
  const int max_row = static_cast<int>(row_count) - 1;
  const int wanted_row = static_cast<int>(current_row) + delta;
  if (edge_policy == VerticalEdgePolicy::kToRowEdge && (wanted_row < 0 || wanted_row > max_row)) {
    const std::size_t edge_row = wanted_row < 0 ? 0 : static_cast<std::size_t>(max_row);
    const WrappedRowLayout edge = WrappedRowAt(edge_row);
    caret.line = edge.line_index;
    const std::size_t line_length = document_->lines.LineLength(caret.line);
    const bool row_table = soft_wrap_ && !layout_cache_.wrapped_row_layouts_trivial();
    if (wanted_row < 0) {
      caret.column = row_table ? TextColumnAtVisualColumn(caret.line, edge.visual_start) : 0;
    } else {
      caret.column = row_table ? std::min(TextColumnAtVisualColumn(caret.line, edge.visual_end),
                                          line_length)
                               : line_length;
    }
    affinity = WrapRowAffinity::kNextRow;
    preferred_column = PreferredColumnForCaret(caret, affinity);
    return;
  }
  const std::size_t target_row = static_cast<std::size_t>(std::clamp(wanted_row, 0, max_row));
  const WrappedRowLayout target = WrappedRowAt(target_row);
  const std::size_t target_visual_column =
      ResolveSoftWrapCursorColumnForTargetRow(preferred_column, target_row);
  caret.line = target.line_index;
  caret.column = TextColumnAtVisualColumn(target.line_index, target_visual_column);
  affinity = AffinityForRowLanding(target_row, target_visual_column);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

}
