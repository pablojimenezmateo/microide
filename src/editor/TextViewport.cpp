#include "editor/TextViewport.h"

#include <algorithm>
#include <cctype>

#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;
constexpr std::size_t kMaxHistoryEntries = 128;
constexpr std::size_t kVisibleLineCacheLimit = 256;
constexpr std::size_t kHighlightCacheLimit = 256;
constexpr std::size_t kHighlightCheckpointInterval = 128;

bool IsCachedHighlightState(const SyntaxState& state) {
  return state.definition_id != 0;
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool TextPositionLess(const TextPosition& lhs, const TextPosition& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line;
  }
  return lhs.column < rhs.column;
}

bool IsIndentCharacter(char c) {
  return c == ' ' || c == '\t';
}

}  // namespace

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "F8 toggles the sidebar, F6 toggles the overlay.\n");
}

bool TextViewport::OpenFile(const std::filesystem::path& path) {
  std::string perf_label = "TextViewport::OpenFile";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(path=" + path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  EnsureDocument();
  const std::optional<std::string> content = util::ReadTextFile(path);
  if (!content.has_value()) {
    return false;
  }

  const util::DecodedText decoded = util::DecodeLines(*content);
  ResetState(decoded.lines, path, decoded.line_ending, decoded.mixed_line_endings,
             DetectEncoding(*content), false, false);
  return true;
}

bool TextViewport::Save() {
  EnsureDocument();
  if (document_->path.empty()) {
    return false;
  }

  const std::string text = util::SerializeLines(document_->lines, document_->line_ending);
  if (!util::WriteTextFileAtomically(document_->path, text)) {
    return false;
  }

  document_->mixed_line_endings = false;
  document_->dirty = false;
  return true;
}

void TextViewport::LoadContent(std::string_view content,
                               const std::filesystem::path& path,
                               std::optional<LineEnding> line_ending) {
  EnsureDocument();
  const util::DecodedText decoded = util::DecodeLines(content);
  ResetState(decoded.lines, path, line_ending.value_or(decoded.line_ending),
             line_ending.has_value() ? false : decoded.mixed_line_endings, DetectEncoding(content),
             false, false);
}

void TextViewport::SetPath(const std::filesystem::path& path) {
  EnsureDocument();
  document_->path = path;
}

void TextViewport::SetDirty(bool dirty) {
  EnsureDocument();
  document_->dirty = dirty;
  if (dirty) {
    document_->placeholder = false;
  }
}

void TextViewport::SetPlaceholderText(std::string text) {
  EnsureDocument();
  ResetState(util::SplitLines(text), {}, LineEnding::LF, false, DetectEncoding(text), true, false);
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  ResetState({""}, {}, LineEnding::LF, false, TextEncoding::ASCII, false, false);
}

void TextViewport::SetViewportSize(std::size_t visible_lines, std::size_t visible_columns) {
  const std::size_t next_visible_lines = std::max<std::size_t>(1, visible_lines);
  const std::size_t next_visible_columns = std::max<std::size_t>(8, visible_columns);
  const bool wrap_width_changed = soft_wrap_ && visible_columns_ != next_visible_columns;
  visible_lines_ = next_visible_lines;
  visible_columns_ = next_visible_columns;
  if (wrap_width_changed) {
    horizontal_scroll_ = 0;
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

void TextViewport::SetTabSize(std::size_t tab_size) {
  tab_size_ = std::clamp<std::size_t>(tab_size, 1, 16);
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_revision_ = 0;
  visible_line_cache_.clear();
  visible_line_cache_order_.clear();
  ClampCursorColumn();
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::SetIndentWidth(std::size_t indent_width) {
  indent_width_ = std::clamp<std::size_t>(indent_width, 1, 16);
}

void TextViewport::SetSoftTabs(bool soft_tabs) {
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
  cursor_line_ = primary.line;
  cursor_column_ = primary.column;

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
  cursor_line_ = primary.line;
  cursor_column_ = primary.column;
  preferred_column_ = PreferredColumnForCaret(primary);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretHorizontal(caret.position, delta);
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineStart(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = 0;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position.column = 0;
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = CurrentLineLength();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
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
  cursor_line_ = std::min(line, document_->lines.size() - 1);
  cursor_column_ =
      TextLayout::ClampTextColumn(document_->lines[cursor_line_], std::min(column, CurrentLineLength()));
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
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
  const std::size_t text_column =
      TextLayout::TextColumnForVisualColumn(document_->lines[clamped_line], visual_column,
                                            tab_size_);
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

void TextViewport::InsertCharacter(char character) {
  if (has_multiple_carets()) {
    const std::string text(1, character);
    (void)ApplyMultiCaretInsert(text, true);
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
  if (document_->undo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->undo_stack.back());
  document_->undo_stack.pop_back();
  entry.after_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, false);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::BuildAppliedEdit");
    last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, false);
  }
  document_->redo_stack.push_back(std::move(entry));
  return true;
}

bool TextViewport::Redo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Redo");
  if (document_->redo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->redo_stack.back());
  document_->redo_stack.pop_back();
  entry.before_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, true);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::BuildAppliedEdit");
    last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, true);
  }
  document_->undo_stack.push_back(std::move(entry));
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
    document_->redo_stack.clear();
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

std::size_t TextViewport::cursor_visual_column() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return TextLayout::VisualColumnForTextColumn(document_->lines[cursor_line_], cursor_column_,
                                               tab_size_);
}

std::string TextViewport::LineEndingLabel() const {
  const std::string base = util::LineEndingLabel(document_->line_ending);
  const std::string upper =
      base == "crlf" ? "CRLF" : base == "cr" ? "CR" : "LF";
  return document_->mixed_line_endings ? "mixed:" + upper : upper;
}

std::string TextViewport::EncodingLabel() const {
  switch (document_->encoding) {
    case TextEncoding::ASCII:
      return "ASCII";
    case TextEncoding::UTF8:
      return "UTF-8";
    case TextEncoding::Bytes:
    default:
      return "Bytes";
  }
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

LayoutLine TextViewport::VisibleWrappedRowLayout(std::size_t visual_row_index) const {
  if (!soft_wrap_) {
    return VisibleLineLayout(visual_row_index);
  }
  EnsureWrappedRowLayouts();
  if (visual_row_index >= wrapped_row_layouts_.size()) {
    return LayoutLine{};
  }

  const WrappedRowLayout& row = wrapped_row_layouts_[visual_row_index];
  LayoutLine layout = TextLayout::BuildVisibleLine(document_->lines[row.line_index], row.visual_start,
                                                   visible_columns_, tab_size_);
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
  if (!soft_wrap_) {
    return WrappedVisualRow{
        .line_index = visual_row_index,
        .visual_start = horizontal_scroll_,
        .visual_end = horizontal_scroll_ + visible_columns_,
    };
  }
  EnsureWrappedRowLayouts();
  if (visual_row_index >= wrapped_row_layouts_.size()) {
    return {};
  }
  const WrappedRowLayout& row = wrapped_row_layouts_[visual_row_index];
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
  if (!soft_wrap_) {
    const std::size_t line = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, visual_row)),
                                                    document_->lines.size() - 1);
    const std::size_t column_visual = static_cast<std::size_t>(std::max(0, visual_col));
    return LogicalPosition{
        .line = line,
        .column = TextLayout::TextColumnForVisualColumn(document_->lines[line], column_visual, tab_size_),
    };
  }
  EnsureWrappedRowLayouts();
  if (wrapped_row_layouts_.empty()) {
    return {};
  }
  const std::size_t clamped_row = std::min<std::size_t>(
      std::max(0, visual_row), wrapped_row_layouts_.size() - 1);
  const WrappedRowLayout& layout = wrapped_row_layouts_[clamped_row];
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
  if (!soft_wrap_) {
    return static_cast<int>(document_->lines.size());
  }
  EnsureWrappedRowLayouts();
  return static_cast<int>(wrapped_row_layouts_.size());
}

std::size_t TextViewport::visual_line_count() const {
  return static_cast<std::size_t>(std::max(0, VisualRowCount()));
}

const std::vector<SyntaxTokenKind>& TextViewport::HighlightedLineTokens(
    std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightedLineTokens");
  static const std::vector<SyntaxTokenKind> kEmptyTokens;
  if (line_index >= document_->lines.size()) {
    return kEmptyTokens;
  }
  if (!syntax_highlighting_enabled()) {
    return kEmptyTokens;
  }

  ++highlight_queries_;
  EnsureHighlightCaches();

  if (const auto it = highlight_cache_.find(line_index); it != highlight_cache_.end()) {
    util::PerformanceTrace::Scope hit_scope("TextViewport::HighlightedLineTokens::CacheHit");
    ++highlight_hits_;
    return it->second;
  }

  util::PerformanceTrace::Scope miss_scope("TextViewport::HighlightedLineTokens::CacheMiss");
  const SyntaxState previous_state = HighlightStateBeforeLine(line_index);
  HighlightedLine highlighted;
  {
    util::PerformanceTrace::Scope highlight_scope(
        "TextViewport::HighlightedLineTokens::HighlightLine");
    highlighted = SyntaxHighlighter::HighlightLine(document_->lines[line_index], document_->path,
                                                   previous_state);
  }
  line_highlight_states_[line_index] = highlighted.end_state;

  if (highlight_cache_.size() >= kHighlightCacheLimit) {
    highlight_cache_.erase(highlight_cache_order_.front());
    highlight_cache_order_.pop_front();
  }
  auto [it, _] = highlight_cache_.emplace(line_index, highlighted.tokens);
  highlight_cache_order_.push_back(line_index);
  return it->second;
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
  });
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return TextPositionLess(lhs.position, rhs.position);
            });
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
    InvalidateDerivedCaches(0);
    InvalidateVisualColumnCache();
    EnsureCursorVisible();
    PushHistoryEntry(BuildHistoryEntryForDocumentChange(before_lines, before_state,
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
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches();
  EnsureCursorVisible();
}

void TextViewport::EnsureInitialHighlightState() const {
  std::string perf_label = "TextViewport::EnsureInitialHighlightState";
  if (util::PerformanceTrace::Enabled() && !document_->path.empty()) {
    perf_label += "(path=" + document_->path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value()) {
    return;
  }
  initial_highlight_state_ = SyntaxHighlighter::InitialState(document_->path, document_->lines);
}

void TextViewport::EnsureHighlightCaches() const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCaches");
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }

  EnsureInitialHighlightState();
  if (highlight_state_revision_ != document_->layout_revision) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
    highlight_checkpoints_.clear();
    highlight_state_revision_ = document_->layout_revision;
  }
  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
  }
  const std::size_t checkpoint_count =
      ((document_->lines.size() - 1) / kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.assign(checkpoint_count, SyntaxState{});
  }
  if (!highlight_checkpoints_.empty()) {
    highlight_checkpoints_.front() = *initial_highlight_state_;
  }
}

void TextViewport::EnsureHighlightCheckpoint(std::size_t checkpoint_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCheckpoint");
  EnsureHighlightCaches();
  if (!syntax_highlighting_enabled() || document_->lines.empty() ||
      checkpoint_index >= highlight_checkpoints_.size()) {
    return;
  }
  if (IsCachedHighlightState(highlight_checkpoints_[checkpoint_index])) {
    return;
  }

  std::size_t previous_checkpoint = checkpoint_index;
  while (previous_checkpoint > 0 &&
         !IsCachedHighlightState(highlight_checkpoints_[previous_checkpoint])) {
    --previous_checkpoint;
  }

  SyntaxState state = previous_checkpoint == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[previous_checkpoint];
  std::size_t line = previous_checkpoint * kHighlightCheckpointInterval;
  const std::size_t target_line =
      std::min(document_->lines.size(), checkpoint_index * kHighlightCheckpointInterval);
  util::PerformanceTrace::Scope replay_scope(
      "TextViewport::EnsureHighlightCheckpoint::ReplayToCheckpoint");
  for (; line < target_line; ++line) {
    if (IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
    } else {
      {
        util::PerformanceTrace::Scope advance_scope(
            "TextViewport::EnsureHighlightCheckpoint::AdvanceState");
        state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
      }
      line_highlight_states_[line] = state;
      ++highlight_checkpoint_advances_;
    }
    const std::size_t next_line = line + 1;
    if (next_line < document_->lines.size() &&
        next_line % kHighlightCheckpointInterval == 0) {
      highlight_checkpoints_[next_line / kHighlightCheckpointInterval] = state;
    }
  }
}

SyntaxState TextViewport::HighlightStateBeforeLine(std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightStateBeforeLine");
  EnsureHighlightCaches();
  if (line_index == 0) {
    return *initial_highlight_state_;
  }

  const std::size_t checkpoint_index = line_index / kHighlightCheckpointInterval;
  EnsureHighlightCheckpoint(checkpoint_index);
  const std::size_t checkpoint_line = checkpoint_index * kHighlightCheckpointInterval;
  SyntaxState state = checkpoint_index == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[checkpoint_index];

  util::PerformanceTrace::Scope replay_scope("TextViewport::HighlightStateBeforeLine::Replay");
  for (std::size_t line = checkpoint_line; line < line_index; ++line) {
    if (IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
      continue;
    }
    {
      util::PerformanceTrace::Scope advance_scope(
          "TextViewport::HighlightStateBeforeLine::AdvanceState");
      state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
    }
    line_highlight_states_[line] = state;
    ++highlight_state_advances_;
  }
  return state;
}

void TextViewport::InvalidateDerivedCaches() {
  InvalidateDerivedCaches(0);
}

void TextViewport::InvalidateDerivedCaches(std::size_t start_line) {
  EnsureDocument();
  ++document_->layout_revision;
  const std::size_t safe_start = std::min(start_line, document_->lines.size());

  if (safe_start == 0) {
    visible_line_cache_.clear();
    visible_line_cache_order_.clear();
    highlight_cache_.clear();
    highlight_cache_order_.clear();
    initial_highlight_state_.reset();
    line_highlight_states_.clear();
    highlight_checkpoints_.clear();
    highlight_state_revision_ = document_->layout_revision;
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
    line_highlight_states_.resize(document_->lines.size(), SyntaxState{});
  }
  for (std::size_t line = safe_start; line < line_highlight_states_.size(); ++line) {
    line_highlight_states_[line] = SyntaxState{};
  }

  const std::size_t checkpoint_count =
      document_->lines.empty() ? 0
                               : ((document_->lines.size() - 1) / kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.resize(checkpoint_count, SyntaxState{});
  }
  const std::size_t checkpoint_start = safe_start / kHighlightCheckpointInterval;
  for (std::size_t index = checkpoint_start; index < highlight_checkpoints_.size(); ++index) {
    highlight_checkpoints_[index] = SyntaxState{};
  }
  if (!highlight_checkpoints_.empty()) {
    EnsureInitialHighlightState();
    highlight_checkpoints_.front() = *initial_highlight_state_;
  }
  highlight_state_revision_ = document_->layout_revision;
}

void TextViewport::InvalidateVisualColumnCache() {
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_revision_ = 0;
  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  wrapped_row_layouts_tab_size_ = 0;
  wrapped_row_layouts_visible_columns_ = 0;
  wrapped_row_layouts_revision_ = 0;
}

void TextViewport::InvalidateLayoutCaches() {
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(0);
}

void TextViewport::InvalidateSyntaxHighlighting() {
  InvalidateLayoutCaches();
}

void TextViewport::RefreshEncoding() {
  document_->encoding = DetectEncoding(document_->lines);
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
  document_->redo_stack.clear();
  document_->undo_stack.push_back(std::move(entry));
  if (document_->undo_stack.size() > kMaxHistoryEntries) {
    document_->undo_stack.pop_front();
  }
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto erase_begin =
      document_->lines.begin() + static_cast<std::ptrdiff_t>(start_line);
  const auto erase_end =
      erase_begin + static_cast<std::ptrdiff_t>(std::min(removed_count, document_->lines.size() - start_line));
  document_->lines.erase(erase_begin, erase_end);

  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;
  document_->lines.insert(document_->lines.begin() + static_cast<std::ptrdiff_t>(start_line),
                          inserted_lines.begin(), inserted_lines.end());
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  RefreshEncoding();
  InvalidateDerivedCaches(start_line);
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

TextViewport::HistoryEntry TextViewport::BuildHistoryEntryForDocumentChange(
    const std::vector<std::string>& before_lines,
    const ViewState& before_state,
    const std::vector<std::string>& after_lines,
    const ViewState& after_state) {
  std::size_t prefix = 0;
  while (prefix < before_lines.size() && prefix < after_lines.size() &&
         before_lines[prefix] == after_lines[prefix]) {
    ++prefix;
  }

  std::size_t before_end = before_lines.size();
  std::size_t after_end = after_lines.size();
  while (before_end > prefix && after_end > prefix &&
         before_lines[before_end - 1] == after_lines[after_end - 1]) {
    --before_end;
    --after_end;
  }

  return HistoryEntry{
      .start_line = prefix,
      .before_lines = SliceLines(before_lines, prefix, before_end),
      .after_lines = SliceLines(after_lines, prefix, after_end),
      .before_state = before_state,
      .after_state = after_state,
  };
}

std::optional<AppliedEdit> TextViewport::BuildAppliedEditForHistoryEntry(
    const TextViewport::HistoryEntry& entry,
    bool forward) {
  const std::vector<std::string>& before_lines = forward ? entry.before_lines : entry.after_lines;
  const std::vector<std::string>& after_lines = forward ? entry.after_lines : entry.before_lines;
  if (before_lines.empty() || after_lines.empty()) {
    return std::nullopt;
  }

  const std::string& before_first = before_lines.front();
  const std::string& after_first = after_lines.front();
  std::size_t common_prefix = 0;
  const std::size_t max_prefix = std::min(before_first.size(), after_first.size());
  while (common_prefix < max_prefix && before_first[common_prefix] == after_first[common_prefix]) {
    ++common_prefix;
  }

  const std::string& before_last = before_lines.back();
  const std::string& after_last = after_lines.back();
  std::size_t common_suffix = 0;
  const std::size_t max_suffix = std::min(before_last.size(), after_last.size());
  while (common_suffix < max_suffix &&
         before_last[before_last.size() - 1 - common_suffix] ==
             after_last[after_last.size() - 1 - common_suffix]) {
    if ((before_lines.size() == 1 && common_prefix + common_suffix >= before_first.size()) ||
        (after_lines.size() == 1 && common_prefix + common_suffix >= after_first.size())) {
      break;
    }
    ++common_suffix;
  }

  std::vector<std::string> replacement_lines = after_lines;
  replacement_lines.front().erase(0, common_prefix);
  if (common_suffix > 0) {
    replacement_lines.back().erase(replacement_lines.back().size() - common_suffix);
  }

  return AppliedEdit{
      .range_before =
          SelectionRange{
              .start =
                  TextPosition{
                      .line = entry.start_line,
                      .column = common_prefix,
                  },
              .end =
                  TextPosition{
                      .line = entry.start_line + before_lines.size() - 1,
                      .column = before_last.size() - common_suffix,
                  },
          },
      .replacement_text = util::SerializeLines(replacement_lines, util::LineEnding::LF),
  };
}

bool TextViewport::ApplyMultiCaretInsert(std::string_view text, bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    const std::string replacement =
        text == "\n" ? "\n" + AutoIndentForNewline(line, column) : std::string(text);
    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(
        SelectionRange{TextPosition{line, column}, TextPosition{line, column}}, replacement);
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

std::string TextViewport::AutoIndentForNewline(std::size_t line, std::size_t column) const {
  if (line >= document_->lines.size()) {
    return {};
  }

  const std::string& current_line = document_->lines[line];
  const std::size_t clamped_column = TextLayout::ClampTextColumn(current_line, column);
  const auto first_non_indent = std::find_if(
      current_line.begin(), current_line.end(),
      [](char c) { return !IsIndentCharacter(c); });
  if (first_non_indent == current_line.end()) {
    return {};
  }

  const std::size_t indent_columns =
      std::min<std::size_t>(static_cast<std::size_t>(first_non_indent - current_line.begin()),
                            clamped_column);
  return current_line.substr(0, indent_columns);
}

bool TextViewport::ApplyMultiCaretBackspace(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column > 0) {
      const std::size_t erase_start =
          TextLayout::PreviousTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, erase_start}, TextPosition{line, column}}, "");
    } else if (line > 0) {
      entry = BuildRangeHistoryEntry(SelectionRange{
                                         TextPosition{line - 1, document_->lines[line - 1].size()},
                                         TextPosition{line, 0},
                                     },
                                     "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyMultiCaretDeleteForward(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column < document_->lines[line].size()) {
      const std::size_t erase_end =
          TextLayout::NextTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line, erase_end}}, "");
    } else if (line + 1 < document_->lines.size()) {
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line + 1, 0}}, "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
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
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(*entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = *entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->redo_stack.clear();
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
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

std::size_t TextViewport::CurrentLineLength() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return document_->lines[cursor_line_].size();
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
  cached_max_visual_columns_revision_ = document_->layout_revision;
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

void TextViewport::EnsureCursorVisible() {
  const std::size_t cursor_visual_row = CursorVisualRow();
  if (cursor_visual_row < scroll_line_) {
    scroll_line_ = cursor_visual_row;
  }

  const std::size_t vertical_margin = std::min(kScrollMargin, visible_lines_ > 0 ? visible_lines_ - 1 : 0);
  if (cursor_visual_row < scroll_line_ + vertical_margin) {
    scroll_line_ = cursor_visual_row > vertical_margin ? cursor_visual_row - vertical_margin : 0;
  } else {
    const std::size_t visible_span = visible_lines_ > vertical_margin ? visible_lines_ - vertical_margin - 1 : 0;
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
    horizontal_scroll_ =
        visual_cursor_column > kHorizontalScrollMargin ? visual_cursor_column - kHorizontalScrollMargin : 0;
    return;
  }

  const std::size_t visible_width = visible_columns_ > 0 ? visible_columns_ : 1;
  const std::size_t horizontal_span =
      visible_width > kHorizontalScrollMargin ? visible_width - kHorizontalScrollMargin - 1 : 0;
  if (visual_cursor_column > horizontal_scroll_ + horizontal_span) {
    horizontal_scroll_ =
        visual_cursor_column > horizontal_span ? visual_cursor_column - horizontal_span : 0;
  }

  ClampScrollState();
}

std::size_t TextViewport::MaxVisualColumns() const {
  if (cached_max_visual_columns_.has_value() &&
      cached_max_visual_columns_tab_size_ == tab_size_ &&
      cached_max_visual_columns_revision_ == document_->layout_revision) {
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
  cached_max_visual_columns_revision_ = document_->layout_revision;
  return *cached_max_visual_columns_;
}

void TextViewport::EnsureWrappedRowLayouts() const {
  if (!document_) {
    return;
  }
  if (!soft_wrap_) {
    wrapped_row_layouts_.clear();
    wrapped_line_row_offsets_.clear();
    wrapped_row_layouts_tab_size_ = tab_size_;
    wrapped_row_layouts_visible_columns_ = visible_columns_;
    wrapped_row_layouts_revision_ = document_->layout_revision;
    return;
  }
  if (wrapped_row_layouts_revision_ == document_->layout_revision &&
      wrapped_row_layouts_tab_size_ == tab_size_ &&
      wrapped_row_layouts_visible_columns_ == visible_columns_) {
    return;
  }

  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  wrapped_row_layouts_.reserve(document_->lines.size());
  wrapped_line_row_offsets_.reserve(document_->lines.size());
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns_);
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
    const std::size_t line_visual_width =
        TextLayout::VisualColumnForTextColumn(document_->lines[line_index],
                                              document_->lines[line_index].size(), tab_size_);
    if (line_visual_width == 0) {
      wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, 0, 0});
      continue;
    }
    for (std::size_t start = 0; start < line_visual_width; start += wrap_columns) {
      const std::size_t end = std::min(line_visual_width, start + wrap_columns);
      wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, start, end});
    }
  }
  if (wrapped_row_layouts_.empty()) {
    wrapped_row_layouts_.push_back(WrappedRowLayout{0, 0, 0});
  }
  wrapped_row_layouts_tab_size_ = tab_size_;
  wrapped_row_layouts_visible_columns_ = visible_columns_;
  wrapped_row_layouts_revision_ = document_->layout_revision;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
}

std::size_t TextViewport::CursorVisualRow() const {
  return CursorVisualRowForCaret(TextPosition{cursor_line_, cursor_column_});
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
  if (wrapped_row_layouts_.empty()) {
    return 0;
  }
  const WrappedRowLayout& row = wrapped_row_layouts_[CursorVisualRowForCaret(caret)];
  return visual >= row.visual_start ? visual - row.visual_start : 0;
}

std::size_t TextViewport::CursorVisualRowForCaret(const TextPosition& caret) const {
  if (!soft_wrap_) {
    return caret.line;
  }
  EnsureWrappedRowLayouts();
  if (document_->lines.empty() || caret.line >= document_->lines.size() ||
      wrapped_line_row_offsets_.size() != document_->lines.size()) {
    return 0;
  }
  const std::size_t base_row = wrapped_line_row_offsets_[caret.line];
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns_);
  const std::size_t caret_visual =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line], caret.column, tab_size_);
  const std::size_t row_in_line = caret_visual / wrap_columns;
  const std::size_t line_visual_width =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line],
                                            document_->lines[caret.line].size(), tab_size_);
  const std::size_t rows_for_line = std::max<std::size_t>(1, (line_visual_width + wrap_columns - 1) /
                                                                 wrap_columns);
  const std::size_t clamped_in_line = std::min(row_in_line, rows_for_line - 1);
  return std::min(base_row + clamped_in_line,
                  wrapped_row_layouts_.empty() ? 0 : wrapped_row_layouts_.size() - 1);
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
  if (wrapped_row_layouts_.empty()) {
    return 0;
  }
  const std::size_t clamped_row = std::min(target_row, wrapped_row_layouts_.size() - 1);
  const WrappedRowLayout& target = wrapped_row_layouts_[clamped_row];
  if (target.visual_end <= target.visual_start) {
    return target.visual_start;
  }
  const std::size_t desired_absolute = target.visual_start + preferred_column;
  return std::min(desired_absolute, target.visual_end);
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
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return TextPositionLess(lhs.position, rhs.position);
            });
  secondary_carets_.erase(
      std::unique(secondary_carets_.begin(), secondary_carets_.end(),
                  [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
                    return lhs.position == rhs.position;
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
  if (soft_wrap_) {
    EnsureWrappedRowLayouts();
    if (wrapped_row_layouts_.empty()) {
      return;
    }
    const std::size_t current_row = CursorVisualRowForCaret(caret);
    const int max_row = static_cast<int>(wrapped_row_layouts_.size()) - 1;
    const std::size_t target_row =
        static_cast<std::size_t>(std::clamp(static_cast<int>(current_row) + delta, 0, max_row));
    const WrappedRowLayout& target = wrapped_row_layouts_[target_row];
    const std::size_t target_visual_column =
        ResolveSoftWrapCursorColumnForTargetRow(caret, preferred_column, target_row);
    caret.line = target.line_index;
    caret.column = TextLayout::TextColumnForVisualColumn(document_->lines[target.line_index],
                                                         target_visual_column, tab_size_);
    return;
  }
  const int current = static_cast<int>(caret.line);
  const int max_index = static_cast<int>(document_->lines.size()) - 1;
  caret.line = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  caret.column = TextLayout::TextColumnForVisualColumn(document_->lines[caret.line], preferred_column,
                                                       tab_size_);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

TextViewport::TextEncoding TextViewport::DetectEncoding(std::string_view content) {
  if (content.find('\0') != std::string_view::npos) {
    return TextEncoding::Bytes;
  }

  const bool ascii_only = std::all_of(content.begin(), content.end(), [](char character) {
    return static_cast<unsigned char>(character) < 0x80;
  });
  if (ascii_only) {
    return TextEncoding::ASCII;
  }

  return util::IsValidUtf8(content) ? TextEncoding::UTF8 : TextEncoding::Bytes;
}

TextViewport::TextEncoding TextViewport::DetectEncoding(const std::vector<std::string>& lines) {
  bool ascii_only = true;
  for (const std::string& line : lines) {
    if (line.find('\0') != std::string::npos) {
      return TextEncoding::Bytes;
    }

    for (char character : line) {
      if (static_cast<unsigned char>(character) >= 0x80) {
        ascii_only = false;
        break;
      }
    }

    if (!ascii_only && !util::IsValidUtf8(line)) {
      return TextEncoding::Bytes;
    }
  }

  return ascii_only ? TextEncoding::ASCII : TextEncoding::UTF8;
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
