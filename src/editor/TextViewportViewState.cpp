#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

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
  InvalidateVisualColumnCache();
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorVertical(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

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
  BeginSelectionIfNeeded(extend_selection);
  PlacePrimaryCaret(cursor_line_, CurrentLineLength());
  for (SecondaryCaret& caret : secondary_carets_) {
    if (caret.position.line < document_->lines.size()) {
      caret.position.column = document_->lines[caret.position.line].size();
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

  BeginSelectionIfNeeded(extend_selection);
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t line_length = document_->lines[clamped_line].size();
  const std::size_t clamped_column = TextLayout::ClampTextColumn(
      document_->lines[clamped_line], std::min(column, line_length));
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
  const std::size_t text_column = TextLayout::TextColumnForVisualColumn(
      document_->lines[clamped_line], visual_column, tab_size_);
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

void TextViewport::Page(int direction) {
  if (direction == 0) {
    return;
  }
  const std::size_t step = visible_lines_ > 1 ? visible_lines_ - 1 : 1;
  MoveCursorVertical(static_cast<int>(step) * direction);
}

std::size_t TextViewport::cursor_visual_column() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return TextLayout::VisualColumnForTextColumn(document_->lines[cursor_line_], cursor_column_,
                                               tab_size_);
}

std::size_t TextViewport::CurrentLineLength() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return document_->lines[cursor_line_].size();
}

void TextViewport::ClampCursorColumn() {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    cursor_column_ = 0;
    return;
  }

  cursor_column_ = TextLayout::TextColumnForVisualColumn(document_->lines[cursor_line_],
                                                         preferred_column_, tab_size_);
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
  const std::size_t visual =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line], caret.column, tab_size_);
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
  const std::size_t caret_visual =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line], caret.column, tab_size_);
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
    return horizontal_scroll_ + preferred_column;
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
  const std::string& line = document_->lines[caret.line];
  caret.column = std::min(caret.column, line.size());
  if (delta < 0) {
    for (int i = delta; i < 0; ++i) {
      caret.column = TextLayout::PreviousTextColumn(line, caret.column);
      if (caret.column == 0) {
        break;
      }
    }
  } else {
    for (int i = 0; i < delta; ++i) {
      caret.column = TextLayout::NextTextColumn(line, caret.column);
      if (caret.column >= line.size()) {
        break;
      }
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
  caret.column = TextLayout::TextColumnForVisualColumn(document_->lines[target.line_index],
                                                       target_visual_column, tab_size_);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

}  // namespace microide::editor
