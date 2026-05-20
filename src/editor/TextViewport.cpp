#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cctype>
#include <limits>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

namespace {

constexpr std::size_t kMaxHistoryEntries = 128;
constexpr std::size_t kVisibleLineCacheLimit = 256;

// `kHighlightCheckpointInterval` is declared in editor/TextViewportInternal.h
// (detail namespace) so the highlight-cache sibling TU sees the same value.

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

// `PositionLess`, `SelectionRangeForSecondaryCaret`, `RangeEndExclusive`,
// `ValidateRangeColumns`, and `TextBetweenLines` live in
// `editor/TextViewportInternal.h` so they are shared with the language-behavior
// sibling translation unit. Use them via `detail::`.

}  // namespace

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "F8 toggles the sidebar, F6 toggles the overlay.\n");
}

void TextViewport::SetPlaceholderText(std::string text) {
  EnsureDocument();
  ResetState(util::SplitLines(text), {}, LineEnding::LF, false, DetectEncoding(text), true, false);
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  ResetState({""}, {}, LineEnding::LF, false, TextEncoding::ASCII, false, false);
}

void TextViewport::InsertCharacter(char character) {
  if (has_multiple_carets()) {
    if (TryMultiCaretPairInsert(character)) {
      return;
    }
    const std::string text(1, character);
    (void)ApplyMultiCaretInsert(text, true);
    return;
  }
  if (TrySurroundInsert(character)) {
    return;
  }
  if (TrySkipOverClose(character)) {
    return;
  }
  (void)MaybeDedentOnClose(character);
  if (TryAutoCloseInsert(character)) {
    return;
  }
  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  const std::string text(1, character);
  (void)ApplyRangeEdit(range, text, true);
}

void TextViewport::InsertText(std::string_view text, bool record_undo) {
  if (text.empty()) {
    return;
  }
  if (text.size() == 1 && text.front() != '\n' && record_undo) {
    InsertCharacter(text.front());
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretInsert(text, record_undo);
    return;
  }

  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  (void)ApplyRangeEdit(range, text, record_undo);
}

void TextViewport::InsertNewline() {
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretInsert("\n", true);
    return;
  }
  if (TryInsertNewlineSplitBraces()) {
    return;
  }
  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  const std::string newline_text =
      "\n" + AutoIndentForNewline(cursor_line_, cursor_column_);
  (void)ApplyRangeEdit(range, newline_text, true);
}

void TextViewport::InsertTab() {
  if (!soft_tabs_) {
    InsertCharacter('\t');
    return;
  }

  const std::size_t safe_indent_width = std::max<std::size_t>(1, indent_width_);
  const std::size_t visual_column = cursor_visual_column();
  const std::size_t remainder = visual_column % safe_indent_width;
  const std::size_t spaces =
      remainder == 0 ? safe_indent_width : safe_indent_width - remainder;
  InsertText(std::string(std::max<std::size_t>(1, spaces), ' '));
}

void TextViewport::Backspace() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Backspace");
  if (document_->lines.empty()) {
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretBackspace(true);
    return;
  }

  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  if (cursor_column_ > 0) {
    const std::size_t erase_start =
        TextLayout::PreviousTextColumn(document_->lines[cursor_line_], cursor_column_);
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, erase_start},
            .end = TextPosition{cursor_line_, cursor_column_},
        },
        "", true);
    return;
  }

  if (cursor_line_ == 0) {
    return;
  }

  (void)ApplyRangeEdit(
      SelectionRange{
          .start = TextPosition{cursor_line_ - 1, document_->lines[cursor_line_ - 1].size()},
          .end = TextPosition{cursor_line_, 0},
      },
      "", true);
}

void TextViewport::DeleteForward() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::DeleteForward");
  if (document_->lines.empty()) {
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretDeleteForward(true);
    return;
  }

  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  const std::string& line = document_->lines[cursor_line_];
  if (cursor_column_ < line.size()) {
    const std::size_t erase_end = TextLayout::NextTextColumn(line, cursor_column_);
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, cursor_column_},
            .end = TextPosition{cursor_line_, erase_end},
        },
        "", true);
    return;
  }

  if (cursor_line_ + 1 >= document_->lines.size()) {
    return;
  }

  (void)ApplyRangeEdit(
      SelectionRange{
          .start = TextPosition{cursor_line_, cursor_column_},
          .end = TextPosition{cursor_line_ + 1, 0},
      },
      "", true);
}

bool TextViewport::Undo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Undo");
  if (undo_history_.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (!undo_history_.CanUndo()) {
    return false;
  }

  HistoryEntry entry = undo_history_.PopUndo();
  entry.after_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, false);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::BuildAppliedEdit");
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, false);
  }
  undo_history_.PushRedo(std::move(entry));
  return true;
}

bool TextViewport::Redo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Redo");
  if (undo_history_.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (!undo_history_.CanRedo()) {
    return false;
  }

  HistoryEntry entry = undo_history_.PopRedo();
  entry.before_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, true);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::BuildAppliedEdit");
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, true);
  }
  undo_history_.PushUndo(std::move(entry));
  return true;
}

bool TextViewport::ReplaceRange(const SelectionRange& range,
                                std::string_view replacement,
                                bool record_undo) {
  return ApplyRangeEdit(range, replacement, record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                const std::vector<std::string>& replacement,
                                bool record_undo) {
  return ApplyLineEdit(start_line, end_line, replacement, record_undo);
}

std::size_t TextViewport::ReplaceAll(std::string_view needle, std::string_view replacement) {
  if (needle.empty() || document_->lines.empty()) {
    return 0;
  }

  const ViewState before_state = CaptureViewState();
  const std::string lowered_needle = ToLower(needle);
  const std::string lowered_replacement = ToLower(replacement);
  std::size_t replacements = 0;
  std::size_t first_changed_line = document_->lines.size();
  std::size_t last_changed_line = 0;
  std::vector<std::string> before_changed_lines;

  // Build the final document state in one pass per line, bypassing ApplyRangeEdit
  // so that InvalidateDerivedCaches / RefreshEncoding are called once at the end
  // rather than once per replacement.
  std::string new_line;
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    std::string& current_line = document_->lines[line_index];
    std::string lowered_line = ToLower(current_line);
    std::size_t offset = lowered_line.find(lowered_needle);
    if (offset == std::string::npos) {
      continue;
    }

    new_line.clear();
    new_line.reserve(current_line.size());
    std::size_t copy_from = 0;
    while (offset != std::string::npos) {
      new_line.append(current_line, copy_from, offset - copy_from);
      new_line.append(replacement);
      copy_from = offset + needle.size();
      ++replacements;
      lowered_line.replace(offset, needle.size(), lowered_replacement);
      offset = lowered_line.find(lowered_needle, offset + replacement.size());
    }
    new_line.append(current_line, copy_from);
    if (first_changed_line == document_->lines.size()) {
      first_changed_line = line_index;
    } else if (line_index > last_changed_line + 1) {
      for (std::size_t gap = last_changed_line + 1; gap < line_index; ++gap) {
        before_changed_lines.push_back(document_->lines[gap]);
      }
    }
    before_changed_lines.push_back(current_line);
    last_changed_line = line_index;
    current_line = std::move(new_line);
  }

  if (replacements > 0) {
    document_->dirty = true;
    undo_history_.ClearRedo();
    RefreshEncoding();
    InvalidateLayoutCaches();
    EnsureCursorVisible();
    const ViewState after_state = CaptureViewState();
    PushHistoryEntry(HistoryEntry{
        .start_line = first_changed_line,
        .before_lines = std::move(before_changed_lines),
        .after_lines = SliceLines(document_->lines, first_changed_line, last_changed_line + 1),
        .before_state = before_state,
        .after_state = after_state,
    });
  }
  return replacements;
}

LayoutLine TextViewport::VisibleLineLayout(std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return LayoutLine{};
  }

  ++visible_line_queries_;
  const VisibleLineCacheKey cache_key{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll_,
      .visible_columns = visible_columns_,
      .tab_size = tab_size_,
  };
  LayoutLine layout;
  if (const auto it = visible_line_cache_.find(cache_key); it != visible_line_cache_.end()) {
    ++visible_line_hits_;
    layout = it->second;
  } else {
    layout = TextLayout::BuildVisibleLine(document_->lines[line_index], horizontal_scroll_,
                                          visible_columns_, tab_size_);
    if (visible_line_cache_.size() >= kVisibleLineCacheLimit) {
      visible_line_cache_.erase(visible_line_cache_order_.front());
      visible_line_cache_order_.pop_front();
    }
    visible_line_cache_.emplace(cache_key, layout);
    visible_line_cache_order_.push_back(cache_key);
  }

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
  if (wrapped_row_layouts_trivial_) {
    return WrappedRowLayout{visual_row_index, horizontal_scroll_,
                            horizontal_scroll_ + visible_columns_};
  }
  if (wrapped_row_layouts_.empty()) {
    return WrappedRowLayout{};
  }
  const std::size_t clamped = std::min<std::size_t>(visual_row_index,
                                                    wrapped_row_layouts_.size() - 1);
  return wrapped_row_layouts_[clamped];
}

std::size_t TextViewport::WrappedRowCount() const {
  if (wrapped_row_layouts_trivial_) {
    return document_ != nullptr ? document_->lines.size() : 0;
  }
  return wrapped_row_layouts_.size();
}

std::size_t TextViewport::WrappedLineRowOffset(std::size_t line_index) const {
  if (wrapped_row_layouts_trivial_) {
    return line_index;
  }
  if (wrapped_line_row_offsets_.empty() || line_index >= wrapped_line_row_offsets_.size()) {
    return 0;
  }
  return wrapped_line_row_offsets_[line_index];
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
  const std::size_t local_max = width;
  const std::size_t clamped_local = std::min<std::size_t>(std::max(0, visual_col), local_max);
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
  if (wrapped_row_layouts_trivial_) {
    if (document_ == nullptr || document_->lines.empty()) {
      return 0;
    }
    return std::min<std::size_t>(line_index, document_->lines.size() - 1);
  }
  if (wrapped_line_row_offsets_.empty()) {
    return 0;
  }
  return wrapped_line_row_offsets_[std::min<std::size_t>(line_index, wrapped_line_row_offsets_.size() - 1)];
}

TextViewportCacheStats TextViewport::CacheStats() const {
  return TextViewportCacheStats{
      .visible_line_queries = visible_line_queries_,
      .visible_line_hits = visible_line_hits_,
      .highlight_queries = highlight_queries_,
      .highlight_hits = highlight_hits_,
      .highlight_state_advances = highlight_state_advances_,
      .highlight_checkpoint_advances = highlight_checkpoint_advances_,
  };
}

void TextViewport::ResetCacheStats() const {
  visible_line_queries_ = 0;
  visible_line_hits_ = 0;
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

    const std::vector<std::string> before_lines = document_->lines;
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
    PushHistoryEntry(TextViewportUndoHistory::BuildEntryForDocumentChange(before_lines, before_state,
                                                        document_->lines, CaptureViewState()));
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
  auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  std::size_t start = col;
  std::size_t end = col;
  if (col < line.size() && is_word_char(line[col])) {
    while (start > 0 && is_word_char(line[start - 1])) {
      --start;
    }
    while (end < line.size() && is_word_char(line[end])) {
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

  auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

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
  if (col < line.size() && is_word_char(line[col])) {
    // Primary caret indexes a word character.
  } else if (col > 0 && is_word_char(line[col - 1])) {
    anchor_col = col - 1;
  } else {
    return std::nullopt;
  }

  std::size_t start = anchor_col;
  std::size_t end = anchor_col;
  while (start > 0 && is_word_char(line[start - 1])) {
    --start;
  }
  while (end < line.size() && is_word_char(line[end])) {
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
    visible_line_cache_.clear();
    visible_line_cache_order_.clear();
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

  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (it->first.line_index >= safe_start) {
      it = visible_line_cache_.erase(it);
    } else {
      ++it;
    }
  }
  visible_line_cache_order_.erase(
      std::remove_if(visible_line_cache_order_.begin(), visible_line_cache_order_.end(),
                     [&](const VisibleLineCacheKey& key) { return key.line_index >= safe_start; }),
      visible_line_cache_order_.end());

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

void TextViewport::InvalidateVisualColumnCache() {
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_content_revision_ = 0;
  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  wrapped_row_layouts_tab_size_ = 0;
  wrapped_row_layouts_visible_columns_ = 0;
  wrapped_row_layouts_layout_shape_revision_ = 0;
  wrapped_row_layouts_soft_wrap_ = false;
  wrapped_row_layouts_folding_model_ = nullptr;
  wrapped_row_layouts_fold_revision_ = 0;
  wrapped_row_layouts_trivial_ = false;
}

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

bool TextViewport::DeleteSelection() {
  const auto range = selection_range();
  if (!range.has_value()) {
    return false;
  }
  return ApplyRangeEdit(*range, "", true);
}

TextViewport::ViewState TextViewport::CaptureViewState() const {
  return ViewState{
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .secondary_carets = secondary_carets_,
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  };
}

void TextViewport::RestoreViewState(const ViewState& state) {
  cursor_line_ = state.cursor_line;
  cursor_column_ = state.cursor_column;
  preferred_column_ = state.preferred_column;
  scroll_line_ = state.scroll_line;
  horizontal_scroll_ = state.horizontal_scroll;
  selection_anchor_ = state.selection_anchor;
  secondary_carets_ = state.secondary_carets;
  document_->placeholder = state.placeholder;
  document_->dirty = state.dirty;
}

void TextViewport::PushHistoryEntry(HistoryEntry entry) {
  undo_history_.RecordEntry(std::move(entry), document_->lines);
}

void TextViewport::PushHistoryEntryDirect(HistoryEntry entry) {
  undo_history_.RecordEntryDirect(std::move(entry));
}

void TextViewport::BeginUndoGroup() { undo_history_.BeginGroup(CaptureViewState()); }

void TextViewport::EndUndoGroup() { FlushActiveUndoGroup(); }

void TextViewport::FlushActiveUndoGroup() {
  std::optional<HistoryEntry> aggregate =
      undo_history_.FinishActiveGroup(document_->lines, CaptureViewState());
  if (!aggregate.has_value()) {
    return;
  }
  PushHistoryEntry(std::move(*aggregate));
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;
  TextViewportUndoHistory::ApplyEntryToLines(document_->lines, entry, forward);

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  RefreshEncoding();
  // Undo/redo replays a content delta starting at start_line.
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, start_line);
  UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_lines);
  EnsureCursorVisible();
}

std::optional<TextViewport::HistoryEntry> TextViewport::BuildRangeHistoryEntry(
    const SelectionRange& range,
    std::string_view replacement) const {
  if (document_->lines.empty()) {
    return std::nullopt;
  }

  const SelectionRange normalized = NormalizeRange(range);
  const auto clamp_position = [&](TextPosition position) {
    const std::size_t line = std::min(position.line, document_->lines.size() - 1);
    return TextPosition{
        .line = line,
        .column = TextLayout::ClampTextColumn(document_->lines[line], position.column),
    };
  };

  const TextPosition start = clamp_position(normalized.start);
  const TextPosition end = clamp_position(normalized.end);
  if (start.line == end.line && start.column == end.column && replacement.empty()) {
    return std::nullopt;
  }

  const std::vector<std::string> before_lines =
      SliceLines(document_->lines, start.line, end.line + 1);
  const std::vector<std::string> replacement_lines =
      util::SplitLines(util::NormalizeLineEndings(replacement));

  std::vector<std::string> after_lines;
  after_lines.reserve(std::max<std::size_t>(1, replacement_lines.size()));
  const std::string prefix = document_->lines[start.line].substr(0, start.column);
  const std::string suffix = document_->lines[end.line].substr(end.column);
  if (replacement_lines.size() == 1) {
    after_lines.push_back(prefix + replacement_lines.front() + suffix);
  } else {
    after_lines.push_back(prefix + replacement_lines.front());
    after_lines.insert(after_lines.end(), replacement_lines.begin() + 1, replacement_lines.end() - 1);
    after_lines.push_back(replacement_lines.back() + suffix);
  }

  ViewState after_state = CaptureViewState();
  after_state.cursor_line = start.line + after_lines.size() - 1;
  after_state.cursor_column =
      after_lines.size() == 1 ? prefix.size() + replacement_lines.front().size()
                              : replacement_lines.back().size();
  after_state.preferred_column = TextLayout::VisualColumnForTextColumn(
      after_lines.back(), after_state.cursor_column, tab_size_);
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  return HistoryEntry{
      .start_line = start.line,
      .before_lines = before_lines,
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewState(),
      .after_state = after_state,
  };
}

TextViewport::HistoryEntry TextViewport::BuildLineHistoryEntry(
    std::size_t start_line,
    std::size_t end_line,
    const std::vector<std::string>& replacement) const {
  const std::size_t clamped_start = std::min(start_line, document_->lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, document_->lines.size());

  std::vector<std::string> after_lines = replacement;
  if (after_lines.empty()) {
    after_lines.push_back("");
  }

  ViewState after_state = CaptureViewState();
  const std::size_t total_after_lines =
      document_->lines.size() - (clamped_end - clamped_start) + after_lines.size();
  after_state.cursor_line = std::min(clamped_start, total_after_lines - 1);
  after_state.cursor_column = 0;
  after_state.preferred_column = 0;
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  return HistoryEntry{
      .start_line = clamped_start,
      .before_lines = SliceLines(document_->lines, clamped_start, clamped_end),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewState(),
      .after_state = after_state,
  };
}

bool TextViewport::ApplyRangeEdit(const SelectionRange& range,
                                  std::string_view replacement,
                                  bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(range, replacement);
  if (!entry.has_value()) {
    last_applied_edit_.reset();
    return false;
  }

  ApplyHistoryEntry(*entry, true);
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(*entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = *entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    undo_history_.ClearRedo();
  }
  return true;
}

bool TextViewport::ApplyLineEdit(std::size_t start_line,
                                 std::size_t end_line,
                                 const std::vector<std::string>& replacement,
                                 bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  const HistoryEntry entry = BuildLineHistoryEntry(start_line, end_line, replacement);
  ApplyHistoryEntry(entry, true);
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    undo_history_.ClearRedo();
  }
  return true;
}

void TextViewport::UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                                    std::size_t removed_count,
                                                    const std::vector<std::string>& inserted_lines) {
  if (cached_max_visual_columns_tab_size_ != tab_size_ ||
      cached_visual_line_columns_.size() != document_->lines.size() - inserted_lines.size() +
                                               removed_count) {
    InvalidateVisualColumnCache();
    return;
  }

  std::vector<std::size_t> inserted_columns;
  inserted_columns.reserve(inserted_lines.size());
  for (const std::string& line : inserted_lines) {
    inserted_columns.push_back(TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size_));
  }

  const std::size_t clamped_start = std::min(start_line, cached_visual_line_columns_.size());
  const std::size_t erase_end = std::min(clamped_start + removed_count, cached_visual_line_columns_.size());
  cached_visual_line_columns_.erase(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(erase_end));
  cached_visual_line_columns_.insert(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      inserted_columns.begin(), inserted_columns.end());

  const bool max_line_erased =
      cached_max_visual_columns_line_index_.has_value() &&
      *cached_max_visual_columns_line_index_ >= clamped_start &&
      *cached_max_visual_columns_line_index_ < clamped_start + removed_count;
  const bool candidate_expands_max =
      std::any_of(inserted_columns.begin(), inserted_columns.end(), [&](std::size_t width) {
        return !cached_max_visual_columns_.has_value() || width >= *cached_max_visual_columns_;
      });
  if (max_line_erased || candidate_expands_max || !cached_max_visual_columns_.has_value()) {
    cached_max_visual_columns_.reset();
    cached_max_visual_columns_line_index_.reset();
  } else if (cached_max_visual_columns_line_index_.has_value() &&
             *cached_max_visual_columns_line_index_ >= clamped_start) {
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(inserted_columns.size()) -
                                 static_cast<std::ptrdiff_t>(removed_count);
    *cached_max_visual_columns_line_index_ = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(*cached_max_visual_columns_line_index_) + delta);
  }
  cached_max_visual_columns_content_revision_ = document_->content_revision;
}

std::size_t TextViewport::MaxVisualColumns() const {
  if (cached_max_visual_columns_.has_value() &&
      cached_max_visual_columns_tab_size_ == tab_size_ &&
      cached_max_visual_columns_content_revision_ == document_->content_revision) {
    return *cached_max_visual_columns_;
  }

  if (cached_max_visual_columns_tab_size_ != tab_size_ ||
      cached_visual_line_columns_.size() != document_->lines.size()) {
    cached_visual_line_columns_.assign(document_->lines.size(), 0);
    for (std::size_t index = 0; index < document_->lines.size(); ++index) {
      cached_visual_line_columns_[index] =
          TextLayout::VisualColumnForTextColumn(document_->lines[index],
                                                document_->lines[index].size(), tab_size_);
    }
  }

  std::size_t max_columns = 0;
  std::size_t max_line = 0;
  for (std::size_t index = 0; index < cached_visual_line_columns_.size(); ++index) {
    if (cached_visual_line_columns_[index] >= max_columns) {
      max_columns = cached_visual_line_columns_[index];
      max_line = index;
    }
  }
  cached_max_visual_columns_ = max_columns;
  cached_max_visual_columns_line_index_ = max_line;
  cached_max_visual_columns_tab_size_ = tab_size_;
  cached_max_visual_columns_content_revision_ = document_->content_revision;
  return *cached_max_visual_columns_;
}

void TextViewport::EnsureWrappedRowLayouts() const {
  if (!document_) {
    return;
  }
  // Probe whether the folding model has any collapsed range. When none exist
  // (the common no-folds path on a freshly-opened large file), skip the
  // per-line `IsLineHidden` query entirely. O(1) via the maintained counter
  // — the previous std::vector<bool> linear scan was paid on every edit.
  const bool has_any_collapsed_fold =
      folding_model_ != nullptr && folding_model_->has_any_collapsed_fold();
  const bool trivial_now = !soft_wrap_ && !has_any_collapsed_fold;

  // Trivial-layout cache: in trivial mode the readers (WrappedRowAt,
  // WrappedRowCount, WrappedLineRowOffset, VisualRowForLine) synthesize from
  // the **current** `horizontal_scroll_`/`visible_columns_`/`tab_size_` —
  // they do not read the cached `wrapped_row_layouts_` vectors. So once
  // trivial, the cache stays valid until trivial_now flips. Earlier we also
  // checked tab_size / visible_columns / folding_model / fold_revision here;
  // those triggered a full rebuild on every keystroke because the folding
  // model bumps its revision on each edit even when nothing is collapsed
  // (round-4 Finding 2).
  if (wrapped_row_layouts_trivial_ && trivial_now) {
    return;
  }

  if (!trivial_now &&
      wrapped_row_layouts_layout_shape_revision_ == document_->layout_shape_revision &&
      wrapped_row_layouts_tab_size_ == tab_size_ &&
      wrapped_row_layouts_visible_columns_ == visible_columns_ &&
      wrapped_row_layouts_soft_wrap_ == soft_wrap_ &&
      wrapped_row_layouts_folding_model_ == folding_model_ &&
      wrapped_row_layouts_fold_revision_ ==
          (folding_model_ != nullptr ? folding_model_->revision() : 0)) {
    return;
  }

  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);

  // Trivial-layout fast path: visual row index equals document line index, the
  // visual window is the same for every row, and no line is hidden. Skip the
  // O(line_count) vector population entirely and have the readers synthesize
  // the row data inline. This is the steady state for a freshly-opened large
  // file with soft-wrap off; the previous implementation paid an O(line_count)
  // rebuild on every keystroke under exactly those conditions.
  if (!soft_wrap_ && !has_any_collapsed_fold) {
    wrapped_row_layouts_trivial_ = true;
    util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits, 0);
    wrapped_row_layouts_tab_size_ = tab_size_;
    wrapped_row_layouts_visible_columns_ = visible_columns_;
    wrapped_row_layouts_layout_shape_revision_ = document_->layout_shape_revision;
    wrapped_row_layouts_soft_wrap_ = soft_wrap_;
    wrapped_row_layouts_folding_model_ = folding_model_;
    wrapped_row_layouts_fold_revision_ =
        folding_model_ != nullptr ? folding_model_->revision() : 0;
#ifndef NDEBUG
    ++wrapped_row_layout_build_count_;
#endif
    return;
  }

  wrapped_row_layouts_trivial_ = false;
  wrapped_row_layouts_.reserve(document_->lines.size());
  wrapped_line_row_offsets_.reserve(document_->lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits,
                              document_->lines.size());
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns_);
  std::size_t last_visible_row = 0;
  if (!soft_wrap_) {
    // Non-soft-wrap with collapsed folds: still need wrapped_line_row_offsets_
    // so we can skip hidden lines, but the row payload is uniform.
    const WrappedRowLayout row_template{0, horizontal_scroll_,
                                         horizontal_scroll_ + visible_columns_};
    for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model_->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      WrappedRowLayout row = row_template;
      row.line_index = line_index;
      wrapped_row_layouts_.push_back(row);
    }
  } else {
    const std::size_t safe_tab_size = std::max<std::size_t>(1, tab_size_);
    for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model_->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();

      const std::string& line_text = document_->lines[line_index];
      if (line_text.empty()) {
        wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, 0, 0});
        continue;
      }

      // Single pass: walk the line tracking the visual column, the last whitespace
      // break opportunity, and the current row's text start. Break before any
      // character that would push the row past `wrap_columns`; prefer breaking
      // after the most recent whitespace if one is available inside the current
      // row. Hard-break inside a long word only when no whitespace fits.
      std::size_t row_start_visual = 0;
      std::size_t row_start_text = 0;
      std::size_t last_break_visual = 0;
      std::size_t last_break_text = 0;
      std::size_t visual = 0;
      std::size_t i = 0;
      const std::size_t line_size = line_text.size();
      while (i < line_size) {
        const unsigned char ch = static_cast<unsigned char>(line_text[i]);
        const std::size_t seq_len = util::Utf8SequenceLength(line_text, i);
        std::size_t next_visual;
        if (ch == '\t') {
          const std::size_t remainder = visual % safe_tab_size;
          next_visual =
              visual + (remainder == 0 ? safe_tab_size : safe_tab_size - remainder);
        } else {
          next_visual = visual + 1;
        }

        // Would this character overflow the current row?
        if (next_visual - row_start_visual > wrap_columns && i > row_start_text) {
          std::size_t break_visual;
          std::size_t break_text;
          if (last_break_text > row_start_text) {
            break_visual = last_break_visual;
            break_text = last_break_text;
          } else {
            // No whitespace boundary in this row — hard break before this char.
            break_visual = visual;
            break_text = i;
          }
          wrapped_row_layouts_.push_back(
              WrappedRowLayout{line_index, row_start_visual, break_visual});
          row_start_visual = break_visual;
          row_start_text = break_text;
          last_break_visual = row_start_visual;
          last_break_text = row_start_text;
          visual = break_visual;
          i = break_text;
          continue;
        }

        visual = next_visual;
        i += seq_len;
        if (ch == ' ' || ch == '\t') {
          last_break_visual = visual;
          last_break_text = i;
        }
      }
      wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, row_start_visual, visual});
    }
  }
  if (wrapped_row_layouts_.empty()) {
    wrapped_row_layouts_.push_back(WrappedRowLayout{0, 0, 0});
    wrapped_line_row_offsets_.assign(document_->lines.size(), 0);
  }
  wrapped_row_layouts_tab_size_ = tab_size_;
  wrapped_row_layouts_visible_columns_ = visible_columns_;
  wrapped_row_layouts_layout_shape_revision_ = document_->layout_shape_revision;
  wrapped_row_layouts_soft_wrap_ = soft_wrap_;
  wrapped_row_layouts_folding_model_ = folding_model_;
  wrapped_row_layouts_fold_revision_ =
      folding_model_ != nullptr ? folding_model_->revision() : 0;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
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
  return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
}

}  // namespace microide::editor
