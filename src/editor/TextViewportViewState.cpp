#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microide::editor {
namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;

}  // namespace

void TextViewport::SetViewportSize(std::size_t visible_lines, std::size_t visible_columns) {
  const std::size_t next_visible_lines = std::max<std::size_t>(1, visible_lines);
  const std::size_t next_visible_columns = std::max<std::size_t>(8, visible_columns);
  const bool wrap_width_changed = soft_wrap_ && visible_columns_ != next_visible_columns;
  visible_lines_ = next_visible_lines;
  visible_columns_ = next_visible_columns;
  if (wrap_width_changed) {
    horizontal_scroll_ = 0;
    if (document_ != nullptr) {
      InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
    }
    return;
  }
  ClampScrollState();
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
  // Cursor / selection first: MoveCursorTo runs EnsureCursorVisible, which we let
  // happen here so the caret is well-placed, then override scroll below.
  if (selection.has_value()) {
    MoveCursorTo(selection->start.line, selection->start.column);
    MoveCursorTo(selection->end.line, selection->end.column, true);
  } else {
    MoveCursorTo(cursor_line, cursor_column);
  }
  // Scroll LAST so it is authoritative. ClampScrollState only reduces against the
  // current (possibly default) viewport size; the first real-size SetViewportSize
  // at render re-clamps safely.
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

  LoadContent(text, path_before, line_ending_before);
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
  layout_cache_.ClearVisibleLineAndMaxColumns();
  if (document_ != nullptr) {
    InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
  }
  ClampCursorColumn();
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
  soft_wrap_ = soft_wrap;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  if (document_ != nullptr) {
    InvalidateDerivedCaches(InvalidationReason::LayoutShape, 0);
  }
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::SetFoldingModel(const FoldingModel* folding_model) {
  if (folding_model_ == folding_model) {
    return;
  }
  folding_model_ = folding_model;
  // Deliberately no InvalidateVisualColumnCache() here. Attaching or detaching a
  // fold model changes which lines are *visible*, never how wide any line is, so
  // the per-line visual-column table and the visible-line text cache it wipes are
  // both still valid. The one layout product that does depend on folds -- the
  // wrapped-row layout -- is already keyed on (folding_model, fold_revision) and
  // rebuilds itself. Wiping the lot cost a full O(lines) recompute of
  // MaxVisualColumns() on the very next line (ClampScrollState reads it): ~10 ms
  // of shell-thread stall on a 50k-line buffer, paid the first time a fold scan
  // resolved a range. Only the vertical mapping moved, so only re-clamp.
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorVertical(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  undo_history_.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  TextPosition primary{cursor_line_, cursor_column_};
  AdvanceCaretVertical(primary, preferred_column_, delta);
  PlacePrimaryCaret(primary.line, primary.column, /*keep_preferred_column=*/true);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretVertical(caret.position, caret.preferred_column, delta);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorHorizontal(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  undo_history_.NotifyCursorMoved();

  // VSCode: a plain (non-extending) Left/Right over a selection collapses to that
  // selection's edge -- the left edge for Left, the right edge for Right -- WITHOUT
  // advancing past it, and does so per caret. The main editor previously cleared
  // the anchor and then stepped one position further, so pressing Right after
  // selecting a word landed the caret one column past the selection instead of at
  // its end. This mirrors SingleLineEditor::MoveLeft/MoveRight. Carets with no
  // selection advance one position as before.
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
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineStart(bool extend_selection) {
  undo_history_.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  PlacePrimaryCaret(cursor_line_, 0);
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position.column = 0;
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  undo_history_.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  PlacePrimaryCaret(cursor_line_, CurrentLineLength());
  for (SecondaryCaret& caret : secondary_carets_) {
    if (caret.position.line < document_->lines.size()) {
      caret.position.column = document_->lines.LineLength(caret.position.line);
    }
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection) {
  if (document_->lines.empty()) {
    return;
  }

  // End the typing coalesce run like every other caret-move method: MoveCursorTo is
  // the path for mouse-click positioning, goto-line, bracket-match, find-nav, and
  // snippet/tabstop jumps. Without this, typing after a click that lands on the exact
  // column the previous run ended at merges both edits into one undo step.
  undo_history_.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t line_length = document_->lines.LineLength(clamped_line);
  const std::size_t clamped_column = TextLayout::ClampTextColumn(
      document_->lines.LineView(clamped_line), std::min(column, line_length));
  PlacePrimaryCaret(clamped_line, clamped_column);
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  EnsureCursorVisible();
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

LineLayoutFacts TextViewport::CachedLineFacts(std::size_t line) const {
  if (document_ == nullptr) {
    return LineLayoutFacts{};
  }
  return layout_cache_.LineFactsIfCurrent(document_->lines.size(), line, tab_size_,
                                          document_->content_revision);
}

std::size_t TextViewport::cursor_visual_column() const {
  return VisualColumnAt(cursor_line_, cursor_column_);
}

std::size_t TextViewport::VisualColumnAt(std::size_t line, std::size_t column) const {
  if (document_->lines.empty() || line >= document_->lines.size()) {
    return 0;
  }
  // A plain-ASCII line spends one cell per byte, so the visual column IS the byte
  // column and there is nothing to walk -- nor any need to materialize the line to
  // find that out. Without this the caret's own visual column costs O(column):
  // on a line with no newlines in it and the caret a megabyte in, that is a
  // megabyte scanned per keystroke by EnsureCursorVisible and again by the
  // preferred-column update (TD-2026-08-05-132).
  const LineLayoutFacts facts = CachedLineFacts(line);
  if (facts.known && facts.plain_ascii) {
    return std::min(column, document_->lines.LineLength(line));
  }
  const std::string_view text = document_->lines.LineView(line);
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes,
                              std::min(column, text.size()));
  return TextLayout::VisualColumnForTextColumn(text, column, tab_size_);
}

std::size_t TextViewport::TextColumnAtVisualColumn(std::size_t line,
                                                   std::size_t visual_column) const {
  if (document_->lines.empty() || line >= document_->lines.size()) {
    return 0;
  }
  // The inverse of the above, and exact for the same reason.
  const LineLayoutFacts facts = CachedLineFacts(line);
  if (facts.known && facts.plain_ascii) {
    return std::min(visual_column, document_->lines.LineLength(line));
  }
  const std::string_view text = document_->lines.LineView(line);
  // Counted the same way: this walk stops at the requested visual column, which on
  // a line of single-cell characters is the same bound as the forward one.
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes,
                              std::min(visual_column, text.size()));
  return TextLayout::TextColumnForVisualColumn(text, visual_column, tab_size_);
}

std::size_t TextViewport::CurrentLineLength() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return document_->lines.LineLength(cursor_line_);
}

void TextViewport::ClampCursorColumn() {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    cursor_column_ = 0;
    return;
  }

  if (soft_wrap_) {
    // In soft-wrap mode preferred_column_ is stored *relative to the wrapped row
    // start* (see PreferredColumnForCaret), so it cannot be fed straight into
    // TextColumnForVisualColumn as an absolute column — that snapped the caret to
    // near the start of the logical line on any continuation row. Resolve it to an
    // absolute visual column for the caret's current row first (mirrors
    // AdvanceCaretVertical).
    const std::size_t target_visual = ResolveSoftWrapCursorColumnForTargetRow(CursorVisualRow());
    cursor_column_ = TextColumnAtVisualColumn(cursor_line_, target_visual);
    return;
  }

  cursor_column_ = TextColumnAtVisualColumn(cursor_line_, preferred_column_);
}

void TextViewport::ClampScrollState() {
  const std::size_t total_visual_lines = visual_line_count();
  const std::size_t max_vertical_scroll =
      total_visual_lines > visible_lines_ ? total_visual_lines - visible_lines_ : 0;
  scroll_line_ = std::min(scroll_line_, max_vertical_scroll);

  if (soft_wrap_) {
    horizontal_scroll_ = 0;
    return;
  }
  const std::size_t max_visual_columns = MaxVisualColumns();
  const std::size_t max_horizontal_scroll =
      max_visual_columns > visible_columns_ ? max_visual_columns - visible_columns_ : 0;
  horizontal_scroll_ = std::min(horizontal_scroll_, max_horizontal_scroll);
}

void TextViewport::PlacePrimaryCaret(std::size_t line,
                                     std::size_t column,
                                     bool keep_preferred_column) {
  cursor_line_ = line;
  cursor_column_ = column;
  if (!keep_preferred_column) {
    preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  }
  caret_navigation_content_revision_ =
      document_ != nullptr ? document_->content_revision : 0;
}

void TextViewport::EnsureCursorVisible() {
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
    return;
  }

  const std::size_t visible_width = visible_columns_ > 0 ? visible_columns_ : 1;
  const std::size_t horizontal_span = visible_width > kHorizontalScrollMargin
                                          ? visible_width - kHorizontalScrollMargin - 1
                                          : 0;
  if (visual_cursor_column > horizontal_scroll_ + horizontal_span) {
    horizontal_scroll_ =
        visual_cursor_column > horizontal_span ? visual_cursor_column - horizontal_span : 0;
  }

  ClampScrollState();
}

std::size_t TextViewport::CursorVisualRow() const {
  return CursorVisualRowForCaret(TextPosition{cursor_line_, cursor_column_});
}

std::size_t TextViewport::cursor_visual_row() const {
  return CursorVisualRow();
}

std::size_t TextViewport::PreferredColumnForCaret(const TextPosition& caret) const {
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
  const WrappedRowLayout row = WrappedRowAt(CursorVisualRowForCaret(caret));
  return visual >= row.visual_start ? visual - row.visual_start : 0;
}

std::size_t TextViewport::CursorVisualRowForCaret(const TextPosition& caret) const {
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
    return base_row;
  }
  if (!soft_wrap_) {
    return base_row;
  }
  // Resolve the caret's visual row by locating, among the rows that actually
  // belong to this line, the one whose [visual_start, visual_end) span owns the
  // caret's visual column. The builder wraps at whitespace, so spans are
  // non-uniform — integer division by the wrap width would land on the wrong
  // row whenever a line breaks before its column limit.
  const std::size_t caret_visual = VisualColumnAt(caret.line, caret.column);
  const auto [first_row, last_row] =
      layout_cache_.WrappedRowRangeForLine(caret.line, document_->lines.size());
  for (std::size_t row = first_row; row < last_row; ++row) {
    if (caret_visual < WrappedRowAt(row).visual_end) {
      return row;
    }
  }
  return last_row;
}

std::size_t TextViewport::ResolveSoftWrapCursorColumnForTargetRow(std::size_t target_row) const {
  return ResolveSoftWrapCursorColumnForTargetRow(
      TextPosition{cursor_line_, cursor_column_}, preferred_column_, target_row);
}

std::size_t TextViewport::ResolveSoftWrapCursorColumnForTargetRow(
    const TextPosition& /*caret*/,
    std::size_t preferred_column,
    std::size_t target_row) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return 0;
  }
  const std::size_t clamped_row = std::min(target_row, row_count - 1);
  const WrappedRowLayout target = WrappedRowAt(clamped_row);
  if (!soft_wrap_) {
    // preferred_column is already an ABSOLUTE visual column (PreferredColumnForCaret
    // returns the absolute visual in the non-wrap case), so it must be returned as
    // is. Adding horizontal_scroll_ double-counted the scroll offset and marched the
    // caret past the intended column — invisible only while horizontal_scroll_ == 0.
    return preferred_column;
  }
  if (target.visual_end <= target.visual_start) {
    return target.visual_start;
  }
  return std::min(target.visual_start + preferred_column, target.visual_end);
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
        // At the start of a line, step back to the end of the previous line
        // (VS Code semantics). Stop only at the very start of the document.
        if (caret.line == 0) {
          break;
        }
        --caret.line;
        caret.column = document_->lines.LineLength(caret.line);
        continue;
      }
      caret.column = TextLayout::PreviousTextColumn(document_->lines.LineView(caret.line), caret.column);
    }
  } else {
    for (int i = 0; i < delta; ++i) {
      const std::string_view line = document_->lines.LineView(caret.line);
      if (caret.column >= line.size()) {
        // At the end of a line, step forward to the start of the next line.
        // Stop only at the very end of the document.
        if (caret.line + 1 >= document_->lines.size()) {
          break;
        }
        ++caret.line;
        caret.column = 0;
        continue;
      }
      caret.column = TextLayout::NextTextColumn(line, caret.column);
    }
  }
}

void TextViewport::DedupeSecondaryCaretsAgainstPrimary() {
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            detail::SecondaryCaretPositionLess);
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
                     [&](const SecondaryCaret& caret) { return caret.position == primary; }),
      secondary_carets_.end());
}

void TextViewport::AdvanceCaretVertical(TextPosition& caret,
                                        std::size_t& preferred_column,
                                        int delta) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return;
  }
  const std::size_t current_row = CursorVisualRowForCaret(caret);
  const int max_row = static_cast<int>(row_count) - 1;
  const std::size_t target_row =
      static_cast<std::size_t>(std::clamp(static_cast<int>(current_row) + delta, 0, max_row));
  const WrappedRowLayout target = WrappedRowAt(target_row);
  const std::size_t target_visual_column =
      ResolveSoftWrapCursorColumnForTargetRow(caret, preferred_column, target_row);
  caret.line = target.line_index;
  caret.column = TextColumnAtVisualColumn(target.line_index, target_visual_column);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

}  // namespace microide::editor
