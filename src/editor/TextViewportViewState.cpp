#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

#include "editor/WordBoundary.h"
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
  // The logical line at the top of the view, resolved against the OLD wrap width
  // (so before visible_columns_ moves). scroll_line_ counts VISUAL rows, and a
  // width change renumbers every one of them: without re-anchoring, widening a
  // soft-wrapped pane scrolled the document to an unrelated place -- and, since
  // this path also skipped ClampScrollState, could leave scroll_line_ past the
  // last row entirely, painting an empty editor until the user scrolled. VS Code
  // keeps the top of the viewport pinned across a re-layout the same way.
  // `scroll_line_ == 0` needs no lookup: row 0 is the first visible line either
  // way, and asking would force a wrapped-row build at the OLD width -- which for
  // a tab whose first real SetViewportSize is also its first wrap width is a
  // whole extra O(document) wrap of a width nothing will ever paint.
  const bool reanchor = wrap_width_changed && document_ != nullptr && scroll_line_ != 0;
  const std::size_t anchor_line = reanchor ? VisualRowLineIndex(scroll_line_) : 0;
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
  // Same re-anchor as a wrap-width change, for the same reason: toggling wrap
  // renumbers every visual row, so the pre-toggle scroll_line_ points at an
  // unrelated part of the document afterwards.
  // Anchoring a view that is already at the top is the identity, and asking costs
  // a wrapped-row build (see SetViewportSize).
  const bool reanchor = document_ != nullptr && scroll_line_ != 0;
  const std::size_t anchor_line = reanchor ? VisualRowLineIndex(scroll_line_) : 0;
  // Whether the caret was on screen decides which of the two anchors wins below.
  // Revealing it unconditionally threw the re-anchor away for the one case that
  // needs it most: wheel-scroll away from the caret, then toggle wrap, and the
  // view snapped back to the caret instead of staying where the reader was.
  //
  // Only asked of a view that is scrolled, and for two reasons: a view at the top
  // has nothing to preserve, and resolving the caret's row in the OLD wrap mode
  // would build that mode's row table -- for EVERY open tab, since a wrap toggle
  // runs this on all of them, including ones no frame has ever painted.
  const bool caret_was_visible = reanchor && [&] {
    const std::size_t caret_row = CursorVisualRow();
    return caret_row >= scroll_line_ && caret_row < scroll_line_ + visible_lines_;
  }();
  soft_wrap_ = soft_wrap;
  // The rows the caret's affinity referred to no longer exist.
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
  WrapRowAffinity primary_affinity = EffectiveCaretAffinity();
  AdvanceCaretVertical(primary, preferred_column_, primary_affinity, delta);
  PlacePrimaryCaret(primary.line, primary.column, /*keep_preferred_column=*/true, primary_affinity);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretVertical(caret.position, caret.preferred_column, caret.wrap_affinity, delta);
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
    // At the line start the step is the line break itself, which is what makes
    // Ctrl+Left walk into the previous line and Ctrl+Backspace join them.
    if (column == 0) {
      return line == 0 ? TextPosition{0, 0}
                       : TextPosition{line - 1, document_->lines.LineLength(line - 1)};
    }
    // LineView, not operator[]: the latter materializes an owned copy of the line
    // into a per-revision cache, which on a file with no line breaks in it is the
    // whole document per keystroke (TD-2026-08-05-133).
    const std::string_view text = document_->lines.LineView(line);
    return TextPosition{line, for_deletion ? DeleteWordBoundaryLeft(text, column)
                                           : WordBoundaryLeft(text, column)};
  }
  if (column >= length) {
    return line + 1 < document_->lines.size() ? TextPosition{line + 1, 0}
                                             : TextPosition{line, length};
  }
  const std::string_view text = document_->lines.LineView(line);
  return TextPosition{line, for_deletion ? DeleteWordBoundaryRight(text, column)
                                         : WordBoundaryRight(text, column)};
}

void TextViewport::MoveCursorWord(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  undo_history_.NotifyCursorMoved();
  // Unlike the character form, a word step over a selection does NOT merely
  // collapse to the selection's edge: VS Code's cursorWordStartLeft /
  // cursorWordEndRight move word-wise from the active caret and let the anchor
  // go, which is also what a GTK/Qt entry does. BeginSelectionIfNeeded drops the
  // anchors on a plain move and seeds them on a Shift move, per caret.
  BeginSelectionIfNeeded(extend_selection);
  const TextPosition primary =
      WordTargetForCaret(TextPosition{cursor_line_, cursor_column_}, delta, /*for_deletion=*/false);
  PlacePrimaryCaret(primary.line, primary.column);

  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position = WordTargetForCaret(caret.position, delta, /*for_deletion=*/false);
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
  // A caret on a fold-hidden line resolves to the OPENER's row, which belongs to
  // another logical line: Home/End must stay on the caret's own line there.
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

// Home. VS Code's `cursorHome`: within the VIEW line (so with word wrap on it
// stops at the start of the wrapped row, not three rows back up the paragraph),
// and to the first non-whitespace character, toggling to the row's true start
// when the caret is already there (TD-2026-08-12-188).
void TextViewport::MoveCursorLineStart(bool extend_selection) {
  undo_history_.NotifyCursorMoved();
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

// End. VS Code's `cursorEnd`: the end of the VIEW line. Landing exactly on a
// wrap point takes kPreviousRow affinity, or the caret would render at the start
// of the NEXT row -- the one position two rows both claim.
void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  undo_history_.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const ViewLineBounds primary_bounds =
      ViewLineBoundsForCaret(TextPosition{cursor_line_, cursor_column_}, EffectiveCaretAffinity());
  PlacePrimaryCaret(cursor_line_, primary_bounds.last, /*keep_preferred_column=*/false,
                    primary_bounds.last_is_wrap_point ? WrapRowAffinity::kPreviousRow
                                                      : WrapRowAffinity::kNextRow);
  for (SecondaryCaret& caret : secondary_carets_) {
    if (caret.position.line < document_->lines.size()) {
      const ViewLineBounds bounds = ViewLineBoundsForCaret(caret.position, caret.wrap_affinity);
      caret.position.column = bounds.last;
      caret.wrap_affinity =
          bounds.last_is_wrap_point ? WrapRowAffinity::kPreviousRow : WrapRowAffinity::kNextRow;
    }
    caret.preferred_column = PreferredColumnForCaret(caret.position, caret.wrap_affinity);
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
    const std::size_t row = CursorVisualRow();
    const std::size_t target_visual =
        ResolveSoftWrapCursorColumnForTargetRow(preferred_column_, row);
    cursor_column_ = TextColumnAtVisualColumn(cursor_line_, target_visual);
    caret_wrap_affinity_ = AffinityForRowLanding(row, target_visual);
    caret_wrap_affinity_position_ = TextPosition{cursor_line_, cursor_column_};
    return;
  }

  cursor_column_ = TextColumnAtVisualColumn(cursor_line_, preferred_column_);
}

void TextViewport::ClampScrollState() {
  // Same argument as the horizontal clamp below: this can only LOWER the offset,
  // and zero is the floor. Reading the visual row count to reach that conclusion
  // builds the whole document's wrapped-row table under soft wrap (or with a
  // collapsed fold) -- an O(document) walk for an answer that is 0 either way,
  // and ClampScrollState runs on every SetViewportSize, i.e. on every paint.
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
    // Nothing to clamp: the clamp below can only lower the offset, and it is
    // already at the floor. Reading MaxVisualColumns() to reach that conclusion
    // builds the whole-document per-line width table -- an O(document) walk for
    // an answer that is 0 either way. Every file opens at column 0, and every
    // restored background tab stays there without any pane ever drawing a
    // horizontal scrollbar, so this was the single most common trigger of that
    // build (TD-2026-08-06-138). The table is still built lazily by whoever
    // genuinely needs it: the scrollbar geometry of a pane that is on screen.
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
  // ON-SCREEN cell, not offset-into-the-row: continuation rows render their
  // content shifted right by the hanging indent, so a preferred column that
  // ignored it moved the caret sideways by the indent width every time vertical
  // motion crossed between a first row and a continuation row.
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
    // The row-offset table stores, for every hidden line, the row where the last
    // VISIBLE line STARTED. Under soft wrap that opener can span several rows, so
    // answering its first row parks a caret that is inside the fold (a search hit,
    // a restored session, a jump-to-definition into a folded body) at the TOP of
    // the opener — and vertical motion then has to walk the opener's own wrapped
    // rows before it can leave a fold it is not even in (TD-2026-08-12-185).
    //
    // The fold's trailing edge is the honest answer: one Down leaves it, which is
    // what the equivalent caret without soft wrap already does.
    if (!soft_wrap_) {
      return base_row;
    }
    const std::size_t opener_line = WrappedRowAt(base_row).line_index;
    return layout_cache_.WrappedRowRangeForLine(opener_line, document_->lines.size()).second;
  }
  if (!soft_wrap_) {
    return base_row;
  }
  // Resolve the caret's visual row by locating, among the rows that actually
  // belong to this line, the one whose [visual_start, visual_end) span owns the
  // caret's visual column. The builder wraps at whitespace, so spans are
  // non-uniform — integer division by the wrap width would land on the wrong
  // row whenever a line breaks before its column limit. `affinity` decides the
  // one column two rows both answer for: the wrap point itself.
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
    // preferred_column is already an ABSOLUTE visual column (PreferredColumnForCaret
    // returns the absolute visual in the non-wrap case), so it must be returned as
    // is. Adding horizontal_scroll_ double-counted the scroll offset and marched the
    // caret past the intended column — invisible only while horizontal_scroll_ == 0.
    return preferred_column;
  }
  if (target.visual_end <= target.visual_start) {
    return target.visual_start;
  }
  // preferred_column counts on-screen cells; the row's first `indent` of them are
  // the hanging-indent gutter, which holds no text. A caret aimed inside the
  // gutter lands on the row's first column, exactly as a click there does.
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
  // The end of the line's LAST row is the end of the line, not a wrap point: no
  // other row answers for it, so there is nothing to disambiguate.
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
                                        WrapRowAffinity& affinity,
                                        int delta) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return;
  }
  const std::size_t current_row = CursorVisualRowForCaret(caret, affinity);
  const int max_row = static_cast<int>(row_count) - 1;
  const std::size_t target_row =
      static_cast<std::size_t>(std::clamp(static_cast<int>(current_row) + delta, 0, max_row));
  const WrappedRowLayout target = WrappedRowAt(target_row);
  const std::size_t target_visual_column =
      ResolveSoftWrapCursorColumnForTargetRow(preferred_column, target_row);
  caret.line = target.line_index;
  caret.column = TextColumnAtVisualColumn(target.line_index, target_visual_column);
  // A preferred column past the target row's last cell lands exactly on that
  // row's wrap point. That position is also the next row's first column, so
  // without the affinity the caret resolves BACK to the row we just left: Up off
  // the last row of a wrapped line did nothing at all, and Down from a wide row
  // skipped the short one under it.
  affinity = AffinityForRowLanding(target_row, target_visual_column);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

}  // namespace microide::editor
