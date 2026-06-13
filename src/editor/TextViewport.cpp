#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <limits>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

// `kHighlightCheckpointInterval` is declared in editor/TextViewportInternal.h
// (detail namespace) so the highlight-cache sibling TU sees the same value.
// `PositionLess`, `SelectionRangeForSecondaryCaret`, `RangeEndExclusive`,
// `ValidateRangeColumns`, and `TextBetweenLines` live in
// `editor/TextViewportInternal.h` so they are shared with the language-behavior
// sibling translation unit. Use them via `detail::`.

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "F8 toggles the sidebar, F6 toggles the overlay.\n");
}

TextViewport::TextViewport(const TextViewport& other)
    : fold_edit_anchor_line_(other.fold_edit_anchor_line_),
      document_(other.document_),
      cursor_line_(other.cursor_line_),
      cursor_column_(other.cursor_column_),
      preferred_column_(other.preferred_column_),
      scroll_line_(other.scroll_line_),
      horizontal_scroll_(other.horizontal_scroll_),
      visible_lines_(other.visible_lines_),
      visible_columns_(other.visible_columns_),
      tab_size_(other.tab_size_),
      indent_width_(other.indent_width_),
      soft_tabs_(other.soft_tabs_),
      soft_wrap_(other.soft_wrap_),
      save_trim_trailing_whitespace_(other.save_trim_trailing_whitespace_),
      save_ensure_final_newline_(other.save_ensure_final_newline_),
      lc_view_(other.lc_view_),
      secondary_carets_(other.secondary_carets_),
      secondary_caret_positions_cache_(other.secondary_caret_positions_cache_),
      layout_cache_(other.layout_cache_),
      highlight_cache_(other.highlight_cache_),
      highlight_cache_order_(other.highlight_cache_order_),
      initial_highlight_state_(other.initial_highlight_state_),
      line_highlight_states_(other.line_highlight_states_),
      line_highlight_states_valid_through_(other.line_highlight_states_valid_through_),
      highlight_checkpoints_(other.highlight_checkpoints_),
      highlight_checkpoints_valid_through_(other.highlight_checkpoints_valid_through_),
      highlight_state_content_revision_(other.highlight_state_content_revision_),
      highlight_state_syntax_revision_(other.highlight_state_syntax_revision_),
      highlight_queries_(other.highlight_queries_),
      highlight_hits_(other.highlight_hits_),
      highlight_state_advances_(other.highlight_state_advances_),
      highlight_checkpoint_advances_(other.highlight_checkpoint_advances_),
      selection_anchor_(other.selection_anchor_),
      last_applied_edit_(other.last_applied_edit_),
      folding_model_(nullptr),
      undo_history_(other.undo_history_) {
  InvalidateVisualColumnCache();
}

TextViewport& TextViewport::operator=(const TextViewport& other) {
  if (this == &other) {
    return *this;
  }
  TextViewport copy(other);
  *this = std::move(copy);
  return *this;
}

TextViewport::TextViewport(TextViewport&& other) noexcept
    : fold_edit_anchor_line_(other.fold_edit_anchor_line_),
      document_(other.document_),
      cursor_line_(other.cursor_line_),
      cursor_column_(other.cursor_column_),
      preferred_column_(other.preferred_column_),
      scroll_line_(other.scroll_line_),
      horizontal_scroll_(other.horizontal_scroll_),
      visible_lines_(other.visible_lines_),
      visible_columns_(other.visible_columns_),
      tab_size_(other.tab_size_),
      indent_width_(other.indent_width_),
      soft_tabs_(other.soft_tabs_),
      soft_wrap_(other.soft_wrap_),
      save_trim_trailing_whitespace_(other.save_trim_trailing_whitespace_),
      save_ensure_final_newline_(other.save_ensure_final_newline_),
      lc_view_(std::move(other.lc_view_)),
      secondary_carets_(std::move(other.secondary_carets_)),
      secondary_caret_positions_cache_(std::move(other.secondary_caret_positions_cache_)),
      layout_cache_(std::move(other.layout_cache_)),
      highlight_cache_(std::move(other.highlight_cache_)),
      highlight_cache_order_(std::move(other.highlight_cache_order_)),
      initial_highlight_state_(std::move(other.initial_highlight_state_)),
      line_highlight_states_(std::move(other.line_highlight_states_)),
      line_highlight_states_valid_through_(other.line_highlight_states_valid_through_),
      highlight_checkpoints_(std::move(other.highlight_checkpoints_)),
      highlight_checkpoints_valid_through_(other.highlight_checkpoints_valid_through_),
      highlight_state_content_revision_(other.highlight_state_content_revision_),
      highlight_state_syntax_revision_(other.highlight_state_syntax_revision_),
      highlight_queries_(other.highlight_queries_),
      highlight_hits_(other.highlight_hits_),
      highlight_state_advances_(other.highlight_state_advances_),
      highlight_checkpoint_advances_(other.highlight_checkpoint_advances_),
      selection_anchor_(std::move(other.selection_anchor_)),
      last_applied_edit_(std::move(other.last_applied_edit_)),
      folding_model_(nullptr),
      undo_history_(std::move(other.undo_history_)) {
  other.folding_model_ = nullptr;
  other.layout_cache_ = TextLayoutCache{};
  other.undo_history_ = TextViewportUndoHistory{};
  InvalidateVisualColumnCache();
}

TextViewport& TextViewport::operator=(TextViewport&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  fold_edit_anchor_line_ = other.fold_edit_anchor_line_;
  document_ = other.document_;
  cursor_line_ = other.cursor_line_;
  cursor_column_ = other.cursor_column_;
  preferred_column_ = other.preferred_column_;
  scroll_line_ = other.scroll_line_;
  horizontal_scroll_ = other.horizontal_scroll_;
  visible_lines_ = other.visible_lines_;
  visible_columns_ = other.visible_columns_;
  tab_size_ = other.tab_size_;
  indent_width_ = other.indent_width_;
  soft_tabs_ = other.soft_tabs_;
  soft_wrap_ = other.soft_wrap_;
  save_trim_trailing_whitespace_ = other.save_trim_trailing_whitespace_;
  save_ensure_final_newline_ = other.save_ensure_final_newline_;
  lc_view_ = std::move(other.lc_view_);
  secondary_carets_ = std::move(other.secondary_carets_);
  secondary_caret_positions_cache_ = std::move(other.secondary_caret_positions_cache_);
  layout_cache_ = std::move(other.layout_cache_);
  highlight_cache_ = std::move(other.highlight_cache_);
  highlight_cache_order_ = std::move(other.highlight_cache_order_);
  initial_highlight_state_ = std::move(other.initial_highlight_state_);
  line_highlight_states_ = std::move(other.line_highlight_states_);
  line_highlight_states_valid_through_ = other.line_highlight_states_valid_through_;
  highlight_checkpoints_ = std::move(other.highlight_checkpoints_);
  highlight_checkpoints_valid_through_ = other.highlight_checkpoints_valid_through_;
  highlight_state_content_revision_ = other.highlight_state_content_revision_;
  highlight_state_syntax_revision_ = other.highlight_state_syntax_revision_;
  highlight_queries_ = other.highlight_queries_;
  highlight_hits_ = other.highlight_hits_;
  highlight_state_advances_ = other.highlight_state_advances_;
  highlight_checkpoint_advances_ = other.highlight_checkpoint_advances_;
  selection_anchor_ = std::move(other.selection_anchor_);
  last_applied_edit_ = std::move(other.last_applied_edit_);
  folding_model_ = nullptr;
  undo_history_ = std::move(other.undo_history_);
  other.folding_model_ = nullptr;
  other.layout_cache_ = TextLayoutCache{};
  other.undo_history_ = TextViewportUndoHistory{};
  InvalidateVisualColumnCache();
  return *this;
}

void TextViewport::SetPlaceholderText(std::string text) {
  EnsureDocument();
  ResetState(util::SplitLines(text), {}, LineEnding::LF, false, DetectEncoding(text), true, false);
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  ResetState({""}, {}, LineEnding::LF, false, TextEncoding::ASCII, false, false);
}

LayoutLine TextViewport::VisibleLineLayout(std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return LayoutLine{};
  }

  LayoutLine layout = layout_cache_.VisibleLineLayoutCached(document_->lines, line_index,
                                                            horizontal_scroll_, visible_columns_,
                                                            tab_size_);

  if (line_index == cursor_line_) {
    const std::size_t caret_visual = TextLayout::VisualColumnForTextColumn(
        document_->lines[line_index], cursor_column_, tab_size_);
    if (caret_visual >= horizontal_scroll_ &&
        caret_visual <= horizontal_scroll_ + visible_columns_) {
      layout.caret_visible = true;
      layout.caret_column = caret_visual - horizontal_scroll_;
    } else {
      layout.caret_visible = false;
      layout.caret_column = 0;
    }
  } else {
    layout.caret_visible = false;
    layout.caret_column = 0;
  }
  return layout;
}

TextViewport::WrappedRowLayout TextViewport::WrappedRowAt(std::size_t visual_row_index) const {
  return layout_cache_.WrappedRowAt(visual_row_index, horizontal_scroll_, visible_columns_);
}

std::size_t TextViewport::WrappedRowCount() const {
  return layout_cache_.WrappedRowCount(document_ != nullptr ? document_->lines.size() : 0);
}

std::size_t TextViewport::WrappedLineRowOffset(std::size_t line_index) const {
  return layout_cache_.WrappedLineRowOffset(line_index);
}

LayoutLine TextViewport::VisibleWrappedRowLayout(std::size_t visual_row_index) const {
  if (!soft_wrap_) {
    return VisibleLineLayout(visual_row_index);
  }
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return LayoutLine{};
  }

  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  const std::size_t row_columns =
      row.visual_end > row.visual_start ? row.visual_end - row.visual_start : 0;
  LayoutLine layout = TextLayout::BuildVisibleLine(
      document_->lines[row.line_index], row.visual_start,
      std::min(visible_columns_, row_columns), tab_size_);
  if (row.line_index == cursor_line_ && visual_row_index == CursorVisualRow()) {
    const std::size_t caret_visual = cursor_visual_column();
    layout.caret_visible = true;
    layout.caret_column = caret_visual >= row.visual_start ? caret_visual - row.visual_start : 0;
  } else {
    layout.caret_visible = false;
    layout.caret_column = 0;
  }
  return layout;
}

TextViewport::WrappedVisualRow TextViewport::WrappedVisualRowLayout(std::size_t visual_row_index) const {
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return {};
  }
  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  return WrappedVisualRow{
      .line_index = row.line_index,
      .visual_start = row.visual_start,
      .visual_end = row.visual_end,
      .indent = row.indent,
  };
}

LogicalPosition TextViewport::LogicalPositionForVisualHit(int visual_row, int visual_col) const {
  if (document_->lines.empty()) {
    return {};
  }
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return {};
  }
  const std::size_t clamped_row =
      std::min<std::size_t>(std::max(0, visual_row), row_count - 1);
  const WrappedRowLayout layout = WrappedRowAt(clamped_row);
  const std::size_t width = layout.visual_end - layout.visual_start;
  // Continuation rows render their content shifted right by `indent` cells, so
  // subtract that gutter before mapping the click to a content column.
  const std::size_t hit_col = static_cast<std::size_t>(std::max(0, visual_col));
  const std::size_t local = hit_col > layout.indent ? hit_col - layout.indent : 0;
  const std::size_t clamped_local = std::min<std::size_t>(local, width);
  const std::size_t target_visual = layout.visual_start + clamped_local;
  return LogicalPosition{
      .line = layout.line_index,
      .column = TextLayout::TextColumnForVisualColumn(document_->lines[layout.line_index],
                                                       target_visual, tab_size_),
  };
}

int TextViewport::VisualRowCount() const {
  EnsureWrappedRowLayouts();
  return static_cast<int>(WrappedRowCount());
}

std::size_t TextViewport::visual_line_count() const {
  return static_cast<std::size_t>(std::max(0, VisualRowCount()));
}

std::size_t TextViewport::VisualRowLineIndex(std::size_t visual_row_index) const {
  EnsureWrappedRowLayouts();
  if (WrappedRowCount() == 0) {
    return 0;
  }
  return WrappedRowAt(visual_row_index).line_index;
}

std::size_t TextViewport::VisualRowForLine(std::size_t line_index) const {
  EnsureWrappedRowLayouts();
  if (document_ == nullptr || document_->lines.empty()) {
    return 0;
  }
  if (layout_cache_.wrapped_row_layouts_trivial()) {
    return std::min<std::size_t>(line_index, document_->lines.size() - 1);
  }
  return layout_cache_.WrappedLineRowOffset(
      std::min<std::size_t>(line_index, document_->lines.size() - 1));
}

TextViewportCacheStats TextViewport::CacheStats() const {
  const TextLayoutCache::Stats layout_stats = layout_cache_.stats();
  return TextViewportCacheStats{
      .visible_line_queries = layout_stats.visible_line_queries,
      .visible_line_hits = layout_stats.visible_line_hits,
      .highlight_queries = highlight_queries_,
      .highlight_hits = highlight_hits_,
      .highlight_state_advances = highlight_state_advances_,
      .highlight_checkpoint_advances = highlight_checkpoint_advances_,
  };
}

void TextViewport::ResetCacheStats() const {
  layout_cache_.ResetStats();
  highlight_queries_ = 0;
  highlight_hits_ = 0;
  highlight_state_advances_ = 0;
  highlight_checkpoint_advances_ = 0;
}

std::vector<TextPosition> TextViewport::secondary_carets() const {
  std::vector<TextPosition> carets;
  carets.reserve(secondary_carets_.size());
  for (const SecondaryCaret& caret : secondary_carets_) {
    carets.push_back(caret.position);
  }
  return carets;
}

std::span<const TextPosition> TextViewport::secondary_caret_positions() const {
  // Quick reject: matching sizes + identical elements means the cache is current and we can
  // hand out the existing view without touching the heap.
  const bool in_sync =
      secondary_caret_positions_cache_.size() == secondary_carets_.size() &&
      std::equal(secondary_carets_.begin(), secondary_carets_.end(),
                 secondary_caret_positions_cache_.begin(),
                 [](const SecondaryCaret& a, const TextPosition& b) { return a.position == b; });
  if (!in_sync) {
    secondary_caret_positions_cache_.clear();
    secondary_caret_positions_cache_.reserve(secondary_carets_.size());
    for (const SecondaryCaret& caret : secondary_carets_) {
      secondary_caret_positions_cache_.push_back(caret.position);
    }
  }
  return secondary_caret_positions_cache_;
}

void TextViewport::AddSecondaryCaret(std::size_t line, std::size_t column) {
  if (document_->lines.empty()) {
    return;
  }
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t clamped_column =
      TextLayout::ClampTextColumn(document_->lines[clamped_line], column);
  const TextPosition position{clamped_line, clamped_column};
  if (position == TextPosition{cursor_line_, cursor_column_}) {
    return;
  }
  if (std::find_if(secondary_carets_.begin(), secondary_carets_.end(),
                   [&](const SecondaryCaret& caret) { return caret.position == position; }) !=
      secondary_carets_.end()) {
    return;
  }
  secondary_carets_.push_back(SecondaryCaret{
      .position = position,
      .preferred_column = PreferredColumnForCaret(position),
      .selection_anchor = std::nullopt,
  });
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return detail::PositionLess(lhs.position, rhs.position);
            });
}

void TextViewport::AddSecondaryCaretWithRange(SelectionRange range) {
  if (document_->lines.empty()) {
    return;
  }
  const SelectionRange norm = NormalizeRange(range);
  if (!detail::ValidateRangeColumns(document_->lines, norm)) {
    return;
  }
  if (norm.start.line == norm.end.line && norm.start.column == norm.end.column) {
    AddSecondaryCaret(norm.start.line, norm.start.column);
    return;
  }
  TextPosition anchor = norm.start;
  TextPosition cursor_end = norm.end;
  if (!detail::PositionLess(anchor, cursor_end)) {
    std::swap(anchor, cursor_end);
  }
  const std::size_t clamped_line = std::min(cursor_end.line, document_->lines.size() - 1);
  cursor_end.column = TextLayout::ClampTextColumn(document_->lines[clamped_line], cursor_end.column);
  anchor.line = std::min(anchor.line, document_->lines.size() - 1);
  anchor.column = TextLayout::ClampTextColumn(document_->lines[anchor.line], anchor.column);

  if (cursor_end == TextPosition{cursor_line_, cursor_column_}) {
    return;
  }
  const SecondaryCaret candidate{.position = cursor_end,
                                 .preferred_column = PreferredColumnForCaret(cursor_end),
                                 .selection_anchor = anchor};
  if (std::find_if(secondary_carets_.begin(), secondary_carets_.end(),
                   [&](const SecondaryCaret& caret) {
                     return caret.position == candidate.position &&
                            caret.selection_anchor == candidate.selection_anchor;
                   }) != secondary_carets_.end()) {
    return;
  }
  secondary_carets_.push_back(candidate);
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return detail::PositionLess(lhs.position, rhs.position);
            });
  DedupeSecondaryCaretsAgainstPrimary();
}

void TextViewport::SetSecondaryCarets(std::vector<TextPosition> carets) {
  secondary_carets_.clear();
  for (const TextPosition& caret : carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
}

void TextViewport::ClearSecondaryCarets() {
  secondary_carets_.clear();
}

void TextViewport::PlaceColumnCaretsBetweenLines(std::size_t anchor_line,
                                                 std::size_t target_line,
                                                 std::size_t column) {
  if (document_->lines.empty()) {
    return;
  }

  const std::size_t lo = std::min(anchor_line, target_line);
  const std::size_t hi = std::max(anchor_line, target_line);

  ClearSecondaryCarets();
  ClearSelection();
  MoveCursorTo(target_line, column, false);

  for (std::size_t line = lo; line <= hi; ++line) {
    if (line == cursor_line_) {
      continue;
    }
    AddSecondaryCaret(line, column);
  }
}

bool TextViewport::has_selection() const {
  return selection_range().has_value();
}

std::optional<SelectionRange> TextViewport::selection_range() const {
  if (!selection_anchor_.has_value()) {
    return std::nullopt;
  }

  const TextPosition cursor{cursor_line_, cursor_column_};
  if (selection_anchor_->line == cursor.line && selection_anchor_->column == cursor.column) {
    return std::nullopt;
  }

  if (IsBefore(*selection_anchor_, cursor)) {
    return SelectionRange{*selection_anchor_, cursor};
  }
  return SelectionRange{cursor, *selection_anchor_};
}

std::string TextViewport::SelectedText() const {
  const auto range = selection_range();
  if (!range.has_value()) {
    return {};
  }

  const auto& start = range->start;
  const auto& end = range->end;
  if (start.line == end.line) {
    return document_->lines[start.line].substr(start.column, end.column - start.column);
  }

  std::size_t total_bytes = document_->lines[start.line].size() - start.column;
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    total_bytes += 1 + document_->lines[line].size();
  }
  total_bytes += 1 + end.column;

  std::string text;
  text.reserve(total_bytes);
  text += document_->lines[start.line].substr(start.column);
  text.push_back('\n');
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    text += document_->lines[line];
    text.push_back('\n');
  }
  text += document_->lines[end.line].substr(0, end.column);
  return text;
}

std::string TextViewport::CurrentLineTextForClipboard() const {
  if (document_->lines.empty()) {
    return {};
  }

  std::string text;
  text.reserve(document_->lines[cursor_line_].size() + 1);
  text += document_->lines[cursor_line_];
  text.push_back('\n');
  return text;
}

bool TextViewport::DeleteSelectedText() {
  return DeleteSelection();
}

bool TextViewport::DeleteCurrentLine() {
  if (document_->lines.empty()) {
    return false;
  }
  if (has_multiple_carets()) {
    std::vector<std::size_t> lines_to_delete;
    lines_to_delete.reserve(secondary_carets_.size() + 1);
    lines_to_delete.push_back(cursor_line_);
    for (const SecondaryCaret& caret : secondary_carets_) {
      lines_to_delete.push_back(std::min(caret.position.line, document_->lines.size() - 1));
    }
    std::sort(lines_to_delete.begin(), lines_to_delete.end());
    lines_to_delete.erase(std::unique(lines_to_delete.begin(), lines_to_delete.end()),
                          lines_to_delete.end());
    if (lines_to_delete.empty()) {
      return false;
    }

    const std::size_t before_document_line_count = document_->lines.size();
    const std::size_t before_lines_start = lines_to_delete.front();
    const std::size_t before_lines_end = lines_to_delete.back() + 1;
    const std::vector<std::string> before_lines =
        SliceLines(document_->lines, before_lines_start, before_lines_end);
    const ViewState before_state = CaptureViewState();
    for (auto it = lines_to_delete.rbegin(); it != lines_to_delete.rend(); ++it) {
      document_->lines.erase(document_->lines.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    if (document_->lines.empty()) {
      document_->lines.push_back("");
    }
    cursor_line_ = std::min(cursor_line_, document_->lines.size() - 1);
    cursor_column_ = 0;
    preferred_column_ = 0;
    selection_anchor_.reset();
    secondary_carets_.clear();
    document_->placeholder = false;
    document_->dirty = true;
    RefreshEncoding();
    // Multi-line delete is a content edit; bump the content tier.
    InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
    InvalidateVisualColumnCache();
    EnsureCursorVisible();
    const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(document_->lines.size()) -
                                      static_cast<std::ptrdiff_t>(before_document_line_count);
    const std::size_t after_slice_size =
        static_cast<std::size_t>(std::max<std::ptrdiff_t>(
            0, static_cast<std::ptrdiff_t>(before_lines.size()) + line_delta));
    const std::size_t after_lines_start = std::min(before_lines_start, document_->lines.size());
    const std::size_t after_lines_end =
        std::min(document_->lines.size(), before_lines_start + after_slice_size);
    const std::vector<std::string> after_lines =
        SliceLines(document_->lines, after_lines_start, after_lines_end);
    HistoryEntry aggregate_entry = TextViewportUndoHistory::BuildEntryForDocumentChange(
        before_lines, before_state, after_lines, CaptureViewState());
    aggregate_entry.start_line += before_lines_start;
    PushHistoryEntry(std::move(aggregate_entry));
    return true;
  }

  if (document_->lines.size() == 1) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{0, 0},
                              .end = TextPosition{0, document_->lines[0].size()},
                          },
                          "", true);
  }

  if (cursor_line_ + 1 < document_->lines.size()) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{cursor_line_, 0},
                              .end = TextPosition{cursor_line_ + 1, 0},
                          },
                          "", true);
  }

  const std::size_t previous_line = cursor_line_ - 1;
  return ApplyRangeEdit(SelectionRange{
                            .start = TextPosition{
                                previous_line,
                                document_->lines[previous_line].size(),
                            },
                            .end = TextPosition{cursor_line_, document_->lines[cursor_line_].size()},
                        },
                        "", true);
}

void TextViewport::ClearSelection() {
  selection_anchor_.reset();
}

void TextViewport::SelectAll() {
  if (document_->lines.empty()) {
    return;
  }

  selection_anchor_ = TextPosition{0, 0};
  cursor_line_ = document_->lines.size() - 1;
  cursor_column_ = document_->lines.back().size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::SelectWordAtCursor() {
  if (document_->lines.empty()) {
    return;
  }
  const std::string& line = document_->lines[cursor_line_];
  const std::size_t col = std::min(cursor_column_, line.size());
  std::size_t start = col;
  std::size_t end = col;
  if (col < line.size() && IsIdentifierByte(line[col])) {
    while (start > 0 && IsIdentifierByte(line[start - 1])) {
      --start;
    }
    while (end < line.size() && IsIdentifierByte(line[end])) {
      ++end;
    }
  }
  if (start == end) {
    return;
  }
  selection_anchor_ = TextPosition{cursor_line_, start};
  cursor_column_ = end;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

std::optional<SelectionRange> TextViewport::OccurrenceSeedSpanForHighlight() const {
  if (document_->lines.empty()) {
    return std::nullopt;
  }


  if (const auto selected = selection_range()) {
    if (selected->start.line != selected->end.line) {
      return std::nullopt;
    }
    const std::size_t line_index = selected->start.line;
    std::size_t start_col = selected->start.column;
    std::size_t end_col = selected->end.column;
    if (start_col > end_col) {
      std::swap(start_col, end_col);
    }
    if (start_col < end_col) {
      return SelectionRange{{line_index, start_col}, {line_index, end_col}};
    }
  }

  const std::size_t line_index = cursor_line_;
  const std::string& line = document_->lines[line_index];
  const std::size_t col = std::min(cursor_column_, line.size());
  std::size_t anchor_col = col;
  if (col < line.size() && IsIdentifierByte(line[col])) {
    // Primary caret indexes a word character.
  } else if (col > 0 && IsIdentifierByte(line[col - 1])) {
    anchor_col = col - 1;
  } else {
    return std::nullopt;
  }

  std::size_t start = anchor_col;
  std::size_t end = anchor_col;
  while (start > 0 && IsIdentifierByte(line[start - 1])) {
    --start;
  }
  while (end < line.size() && IsIdentifierByte(line[end])) {
    ++end;
  }
  if (start >= end) {
    return std::nullopt;
  }
  return SelectionRange{{line_index, start}, {line_index, end}};
}

void TextViewport::SelectLineAtCursor() {
  if (document_->lines.empty()) {
    return;
  }
  selection_anchor_ = TextPosition{cursor_line_, 0};
  cursor_column_ = document_->lines[cursor_line_].size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::ResetState(std::vector<std::string> lines,
                              const std::filesystem::path& path,
                              LineEnding line_ending,
                              bool mixed_line_endings,
                              TextEncoding encoding,
                              bool placeholder,
                              bool dirty) {
  EnsureDocument();
  document_->path = path;
  document_->lines = lines.empty() ? std::vector<std::string>{""} : std::move(lines);
  document_->line_ending = line_ending;
  document_->mixed_line_endings = mixed_line_endings;
  document_->encoding = encoding;
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  secondary_carets_.clear();
  undo_history_.Clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  InvalidateVisualColumnCache();
  // ResetState replaces every line; classify as a content edit so the
  // content tier reflects the change and dependent caches are dropped.
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
  // ResetState is a fresh load, not an in-place edit; clear the fold edit
  // anchor to the idle sentinel so the next user edit publishes its own
  // first-touched line instead of being masked by the wholesale reset.
  fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
  EnsureCursorVisible();
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason) {
  InvalidateDerivedCaches(reason, 0);
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line) {
  EnsureDocument();
  // Tier fan-out: each reason bumps exactly the tiers it implies. Every
  // reason bumps presentation_revision because any cause of invalidation
  // changes at least the rendered pixels. Counters are incremented per tier
  // bump so a smoke run can assert which tiers moved.
  ++document_->presentation_revision;
  util::AddPerformanceCounter(util::PerfCounterId::EditorPresentationRevisionBumps);
  if (reason == InvalidationReason::ContentEdit) {
    ++document_->content_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorContentRevisionBumps);
  } else if (reason == InvalidationReason::SyntaxConfig) {
    ++document_->syntax_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorSyntaxRevisionBumps);
  } else if (reason == InvalidationReason::LayoutShape) {
    ++document_->layout_shape_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorLayoutShapeRevisionBumps);
  }

  // Caches below are content-derived (visible-line layout, highlight tokens,
  // fold edit anchor). They only need rebuilding on a content edit; pure
  // syntax/layout-shape/presentation invalidations do not touch them.
  if (reason != InvalidationReason::ContentEdit) {
    if (reason == InvalidationReason::SyntaxConfig) {
      // Highlight cache depends on syntax_revision. Drop it fully — the
      // start_line argument is meaningless for a theme/contract change.
      highlight_cache_.clear();
      highlight_cache_order_.clear();
      initial_highlight_state_.reset();
      line_highlight_states_.clear();
      line_highlight_states_valid_through_ = 0;
      highlight_checkpoints_.clear();
      highlight_checkpoints_valid_through_ = 0;
      highlight_state_content_revision_ = document_->content_revision;
      highlight_state_syntax_revision_ = document_->syntax_revision;
    }
    return;
  }

  const std::size_t safe_start = std::min(start_line, document_->lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesCalls);
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesLines,
                              document_->lines.size() - safe_start);

  // Folding incremental scan anchors at lines >= fold_edit_anchor_line_. Zero
  // forces a bracket rescan without prefix reuse; SIZE_MAX sentinel means idle.
  if (safe_start == 0) {
    fold_edit_anchor_line_ = 0;
  } else if (safe_start >= document_->lines.size()) {
    fold_edit_anchor_line_ = document_->lines.size();
  } else {
    fold_edit_anchor_line_ = std::min(fold_edit_anchor_line_, safe_start);
  }

  if (safe_start == 0) {
    layout_cache_.ClearVisibleLineAndMaxColumns();
    highlight_cache_.clear();
    highlight_cache_order_.clear();
    initial_highlight_state_.reset();
    line_highlight_states_.clear();
    line_highlight_states_valid_through_ = 0;
    highlight_checkpoints_.clear();
    highlight_checkpoints_valid_through_ = 0;
    highlight_state_content_revision_ = document_->content_revision;
    highlight_state_syntax_revision_ = document_->syntax_revision;
    return;
  }

  layout_cache_.InvalidateVisibleLineCacheFrom(safe_start);

  for (auto it = highlight_cache_.begin(); it != highlight_cache_.end();) {
    if (it->first >= safe_start) {
      it = highlight_cache_.erase(it);
    } else {
      ++it;
    }
  }
  highlight_cache_order_.erase(
      std::remove_if(highlight_cache_order_.begin(), highlight_cache_order_.end(),
                     [&](std::size_t line_index) { return line_index >= safe_start; }),
      highlight_cache_order_.end());

  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.resize(document_->lines.size());
  }
  // Lazy invalidation: drop the validity cursor instead of looping
  // SyntaxState{} into ~50 000 entries on every keystroke.
  line_highlight_states_valid_through_ =
      std::min(line_highlight_states_valid_through_, safe_start);

  const std::size_t checkpoint_count =
      document_->lines.empty() ? 0
                               : ((document_->lines.size() - 1) / detail::kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.resize(checkpoint_count);
  }
  const std::size_t checkpoint_start = safe_start / detail::kHighlightCheckpointInterval;
  highlight_checkpoints_valid_through_ =
      std::min(highlight_checkpoints_valid_through_, checkpoint_start);
  if (!highlight_checkpoints_.empty()) {
    EnsureInitialHighlightState();
    highlight_checkpoints_.front() = *initial_highlight_state_;
    if (highlight_checkpoints_valid_through_ < 1) {
      highlight_checkpoints_valid_through_ = 1;
    }
  }
  highlight_state_content_revision_ = document_->content_revision;
  highlight_state_syntax_revision_ = document_->syntax_revision;
}

std::size_t TextViewport::ConsumeFoldEditAnchorLine() {
  const std::size_t v = fold_edit_anchor_line_;
  fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
  return v;
}

void TextViewport::InvalidateVisualColumnCache() { layout_cache_.InvalidateAll(); }

void TextViewport::InvalidateLayoutCaches() {
  // The only remaining caller (ReplaceAll) just mutated buffer content, so
  // route this through the content tier. We still wipe the visual-column
  // cache because the column metrics depend on the new line widths.
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
}

void TextViewport::InvalidateSyntaxHighlighting() {
  // External callers invoke this when language identification or theme
  // configuration changes — a syntax-tier mutation, not a content edit.
  // The visual-column cache and wrapped-row layouts are unaffected by a
  // syntax change, so leave them in place.
  InvalidateDerivedCaches(InvalidationReason::SyntaxConfig, 0);
}

void TextViewport::BeginSelectionIfNeeded(bool extend_selection) {
  if (extend_selection) {
    if (!selection_anchor_.has_value()) {
      selection_anchor_ = TextPosition{cursor_line_, cursor_column_};
    }
    return;
  }
  selection_anchor_.reset();
}

void TextViewport::UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                                    std::size_t removed_count,
                                                    const std::vector<std::string>& inserted_lines) {
  layout_cache_.UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_lines,
                                                  document_->lines, tab_size_,
                                                  document_->content_revision);
}

std::size_t TextViewport::MaxVisualColumns() const {
  return layout_cache_.MaxVisualColumns(document_->lines, tab_size_, document_->content_revision);
}

void TextViewport::EnsureWrappedRowLayouts() const {
  if (!document_) {
    return;
  }
  layout_cache_.EnsureWrappedRowLayouts(document_->lines, tab_size_, visible_columns_, soft_wrap_,
                                        folding_model_, document_->layout_shape_revision);
}

std::vector<std::string> TextViewport::SliceLines(const std::vector<std::string>& lines,
                                                  std::size_t start_line,
                                                  std::size_t end_line) {
  const std::size_t clamped_start = std::min(start_line, lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, lines.size());
  return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(clamped_start),
                                  lines.begin() + static_cast<std::ptrdiff_t>(clamped_end));
}

SelectionRange TextViewport::NormalizeRange(const SelectionRange& range) {
  return IsBefore(range.start, range.end) ? range : SelectionRange{range.end, range.start};
}

bool TextViewport::IsBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return detail::PositionLess(lhs, rhs);
}

}  // namespace microide::editor
