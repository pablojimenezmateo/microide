#include "editor/TextViewport.h"

#include <algorithm>
#include <cctype>

#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;
constexpr std::size_t kMaxHistoryEntries = 128;
constexpr std::size_t kVisibleLineCacheLimit = 256;
constexpr std::size_t kHighlightCacheLimit = 256;

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::string NormalizeLineEndings(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char character : text) {
    if (character != '\r') {
      normalized.push_back(character);
    }
  }
  return normalized;
}

std::string_view LineEndingText(TextViewport::LineEnding line_ending) {
  switch (line_ending) {
    case TextViewport::LineEnding::CRLF:
      return "\r\n";
    case TextViewport::LineEnding::CR:
      return "\r";
    case TextViewport::LineEnding::LF:
    default:
      return "\n";
  }
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
  EnsureDocument();
  const std::optional<std::string> content = util::ReadTextFile(path);
  if (!content.has_value()) {
    return false;
  }

  const DecodedDocument decoded = DecodeDocument(*content);
  ResetState(decoded.lines, path, decoded.line_ending, decoded.mixed_line_endings,
             decoded.encoding, false, false);
  return true;
}

bool TextViewport::Save() {
  EnsureDocument();
  if (document_->path.empty()) {
    return false;
  }

  const std::string text =
      util::JoinLines(document_->lines, LineEndingText(document_->line_ending));
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
  const DecodedDocument decoded = DecodeDocument(content);
  ResetState(decoded.lines, path, line_ending.value_or(decoded.line_ending),
             line_ending.has_value() ? false : decoded.mixed_line_endings, decoded.encoding,
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
  visible_lines_ = std::max<std::size_t>(1, visible_lines);
  visible_columns_ = std::max<std::size_t>(8, visible_columns);
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

void TextViewport::MoveCursorVertical(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  BeginSelectionIfNeeded(extend_selection);
  const int current = static_cast<int>(cursor_line_);
  const int max_index = static_cast<int>(document_->lines.size()) - 1;
  cursor_line_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  ClampCursorColumn();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorHorizontal(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  BeginSelectionIfNeeded(extend_selection);
  const std::string& line = document_->lines[cursor_line_];
  if (delta < 0) {
    for (int i = delta; i < 0; ++i) {
      cursor_column_ = TextLayout::PreviousTextColumn(line, cursor_column_);
      if (cursor_column_ == 0) {
        break;
      }
    }
  } else {
    for (int i = 0; i < delta; ++i) {
      const std::size_t next_column = TextLayout::NextTextColumn(line, cursor_column_);
      cursor_column_ = next_column;
      if (cursor_column_ >= line.size()) {
        break;
      }
    }
  }
  preferred_column_ = cursor_visual_column();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineStart(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = 0;
  preferred_column_ = 0;
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = CurrentLineLength();
  preferred_column_ = cursor_visual_column();
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
  preferred_column_ = cursor_visual_column();
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
  const int max_index = static_cast<int>(
      document_->lines.size() > visible_lines_ ? document_->lines.size() - visible_lines_ : 0);
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

  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  (void)ApplyRangeEdit(range, text, record_undo);
}

void TextViewport::InsertNewline() {
  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  (void)ApplyRangeEdit(range, "\n", true);
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
  if (document_->lines.empty()) {
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
  if (document_->lines.empty()) {
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
  if (document_->undo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->undo_stack.back());
  document_->undo_stack.pop_back();
  entry.after_state = CaptureViewState();
  ApplyHistoryEntry(entry, false);
  document_->redo_stack.push_back(std::move(entry));
  return true;
}

bool TextViewport::Redo() {
  if (document_->redo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->redo_stack.back());
  document_->redo_stack.pop_back();
  entry.before_state = CaptureViewState();
  ApplyHistoryEntry(entry, true);
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

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const std::string lowered_needle = ToLower(needle);
  std::size_t replacements = 0;

  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    std::string lowered_line = ToLower(document_->lines[line_index]);
    std::size_t offset = lowered_line.find(lowered_needle);
    while (offset != std::string::npos) {
      const SelectionRange range{
          .start = TextPosition{line_index, offset},
          .end = TextPosition{line_index, offset + needle.size()},
      };
      (void)ApplyRangeEdit(range, replacement, false);
      ++replacements;
      lowered_line = ToLower(document_->lines[line_index]);
      offset = lowered_line.find(lowered_needle,
                                 offset + replacement.size());
    }
  }

  if (replacements > 0) {
    PushHistoryEntry(BuildHistoryEntryForDocumentChange(
        before_lines, before_state, document_->lines, CaptureViewState()));
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
  const std::string base =
      document_->line_ending == LineEnding::CRLF ? "CRLF"
      : document_->line_ending == LineEnding::CR ? "CR"
                                                 : "LF";
  return document_->mixed_line_endings ? "mixed:" + base : base;
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
  const std::size_t caret_column =
      line_index == cursor_line_ ? cursor_column_ : document_->lines[line_index].size();
  const auto cached = std::find_if(
      visible_line_cache_.begin(), visible_line_cache_.end(),
      [&](const VisibleLineCacheEntry& entry) {
        return entry.revision == document_->layout_revision && entry.line_index == line_index &&
               entry.horizontal_scroll == horizontal_scroll_ &&
               entry.visible_columns == visible_columns_ && entry.tab_size == tab_size_ &&
               entry.caret_text_column == caret_column;
      });
  if (cached != visible_line_cache_.end()) {
    ++visible_line_hits_;
    return cached->layout;
  }

  LayoutLine layout = TextLayout::BuildVisibleLine(document_->lines[line_index], horizontal_scroll_,
                                                   visible_columns_, caret_column, tab_size_);
  visible_line_cache_.push_back(VisibleLineCacheEntry{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll_,
      .visible_columns = visible_columns_,
      .tab_size = tab_size_,
      .caret_text_column = caret_column,
      .revision = document_->layout_revision,
      .layout = layout,
  });
  if (visible_line_cache_.size() > kVisibleLineCacheLimit) {
    visible_line_cache_.erase(visible_line_cache_.begin(),
                              visible_line_cache_.begin() +
                                  static_cast<std::ptrdiff_t>(visible_line_cache_.size() -
                                                              kVisibleLineCacheLimit));
  }
  return layout;
}

const std::vector<SyntaxTokenKind>& TextViewport::HighlightedLineTokens(
    std::size_t line_index) const {
  static const std::vector<SyntaxTokenKind> kEmptyTokens;
  if (line_index >= document_->lines.size()) {
    return kEmptyTokens;
  }
  if (!syntax_highlighting_enabled()) {
    return kEmptyTokens;
  }

  ++highlight_queries_;
  EnsureInitialHighlightState();
  if (line_index > 0) {
    EnsureHighlightStatesThrough(line_index - 1);
  } else if (highlight_state_revision_ != document_->layout_revision ||
             line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
    highlight_state_computed_through_.reset();
    highlight_state_revision_ = document_->layout_revision;
  }

  const auto cached = std::find_if(
      highlight_cache_.begin(), highlight_cache_.end(), [&](const HighlightCacheEntry& entry) {
        return entry.revision == document_->layout_revision && entry.line_index == line_index;
      });
  if (cached != highlight_cache_.end()) {
    ++highlight_hits_;
    return cached->tokens;
  }

  const SyntaxState previous_state =
      line_index == 0 ? *initial_highlight_state_ : line_highlight_states_[line_index - 1];
  const HighlightedLine highlighted =
      SyntaxHighlighter::HighlightLine(document_->lines[line_index], document_->path,
                                       previous_state);
  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
  }
  line_highlight_states_[line_index] = highlighted.end_state;
  if (!highlight_state_computed_through_.has_value() ||
      line_index == *highlight_state_computed_through_ + 1) {
    highlight_state_computed_through_ = line_index;
  }

  highlight_cache_.push_back(HighlightCacheEntry{
      .line_index = line_index,
      .revision = document_->layout_revision,
      .tokens = highlighted.tokens,
  });
  if (highlight_cache_.size() > kHighlightCacheLimit) {
    highlight_cache_.erase(highlight_cache_.begin(),
                           highlight_cache_.begin() +
                               static_cast<std::ptrdiff_t>(highlight_cache_.size() -
                                                           kHighlightCacheLimit));
  }
  return highlight_cache_.back().tokens;
}

TextViewportCacheStats TextViewport::CacheStats() const {
  return TextViewportCacheStats{
      .visible_line_queries = visible_line_queries_,
      .visible_line_hits = visible_line_hits_,
      .highlight_queries = highlight_queries_,
      .highlight_hits = highlight_hits_,
  };
}

void TextViewport::ResetCacheStats() const {
  visible_line_queries_ = 0;
  visible_line_hits_ = 0;
  highlight_queries_ = 0;
  highlight_hits_ = 0;
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

  std::string text = document_->lines[start.line].substr(start.column);
  text.push_back('\n');
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    text += document_->lines[line];
    text.push_back('\n');
  }
  text += document_->lines[end.line].substr(0, end.column);
  return text;
}

bool TextViewport::DeleteSelectedText() {
  return DeleteSelection();
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
  preferred_column_ = cursor_visual_column();
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
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  InvalidateLayoutCaches();
  EnsureCursorVisible();
}

void TextViewport::EnsureInitialHighlightState() const {
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value()) {
    return;
  }
  initial_highlight_state_ = SyntaxHighlighter::InitialState(document_->path, document_->lines);
}

void TextViewport::EnsureHighlightStatesThrough(std::size_t line_index) const {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }

  EnsureInitialHighlightState();
  if (highlight_state_revision_ != document_->layout_revision ||
      line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
    highlight_state_computed_through_.reset();
    highlight_state_revision_ = document_->layout_revision;
  }

  const std::size_t target_line = std::min(line_index, document_->lines.size() - 1);
  const std::size_t start_line = highlight_state_computed_through_.has_value()
                                     ? *highlight_state_computed_through_ + 1
                                     : 0;
  if (start_line > target_line) {
    return;
  }

  SyntaxState previous_state =
      start_line == 0 ? *initial_highlight_state_ : line_highlight_states_[start_line - 1];
  for (std::size_t line = start_line; line <= target_line; ++line) {
    previous_state =
        SyntaxHighlighter::HighlightLine(document_->lines[line], document_->path, previous_state)
            .end_state;
    line_highlight_states_[line] = previous_state;
  }
  highlight_state_computed_through_ = target_line;
}

void TextViewport::InvalidateLayoutCaches() {
  EnsureDocument();
  ++document_->layout_revision;
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_revision_ = 0;
  visible_line_cache_.clear();
  highlight_cache_.clear();
  initial_highlight_state_.reset();
  line_highlight_states_.clear();
  highlight_state_computed_through_.reset();
  highlight_state_revision_ = document_->layout_revision;
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
  document_->placeholder = state.placeholder;
  document_->dirty = state.dirty;
}

void TextViewport::PushHistoryEntry(HistoryEntry entry) {
  document_->redo_stack.clear();
  document_->undo_stack.push_back(std::move(entry));
  if (document_->undo_stack.size() > kMaxHistoryEntries) {
    document_->undo_stack.erase(document_->undo_stack.begin());
  }
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t remove_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto erase_begin =
      document_->lines.begin() + static_cast<std::ptrdiff_t>(start_line);
  const auto erase_end =
      erase_begin + static_cast<std::ptrdiff_t>(std::min(remove_count, document_->lines.size() - start_line));
  document_->lines.erase(erase_begin, erase_end);

  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;
  document_->lines.insert(document_->lines.begin() + static_cast<std::ptrdiff_t>(start_line),
                          inserted_lines.begin(), inserted_lines.end());
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  RefreshEncoding();
  InvalidateLayoutCaches();
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
  const std::vector<std::string> replacement_lines = util::SplitLines(NormalizeLineEndings(replacement));

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

bool TextViewport::ApplyRangeEdit(const SelectionRange& range,
                                  std::string_view replacement,
                                  bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(range, replacement);
  if (!entry.has_value()) {
    return false;
  }

  ApplyHistoryEntry(*entry, true);
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

void TextViewport::ClampCursorColumn() {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    cursor_column_ = 0;
    return;
  }

  cursor_column_ = TextLayout::TextColumnForVisualColumn(document_->lines[cursor_line_],
                                                         preferred_column_, tab_size_);
}

void TextViewport::ClampScrollState() {
  const std::size_t max_vertical_scroll =
      document_->lines.size() > visible_lines_ ? document_->lines.size() - visible_lines_ : 0;
  scroll_line_ = std::min(scroll_line_, max_vertical_scroll);

  const std::size_t max_visual_columns = MaxVisualColumns();
  const std::size_t max_horizontal_scroll =
      max_visual_columns > visible_columns_ ? max_visual_columns - visible_columns_ : 0;
  horizontal_scroll_ = std::min(horizontal_scroll_, max_horizontal_scroll);
}

void TextViewport::EnsureCursorVisible() {
  if (cursor_line_ < scroll_line_) {
    scroll_line_ = cursor_line_;
  }

  const std::size_t vertical_margin = std::min(kScrollMargin, visible_lines_ > 0 ? visible_lines_ - 1 : 0);
  if (cursor_line_ < scroll_line_ + vertical_margin) {
    scroll_line_ = cursor_line_ > vertical_margin ? cursor_line_ - vertical_margin : 0;
  } else {
    const std::size_t visible_span = visible_lines_ > vertical_margin ? visible_lines_ - vertical_margin - 1 : 0;
    if (cursor_line_ > scroll_line_ + visible_span) {
      scroll_line_ = cursor_line_ > visible_span ? cursor_line_ - visible_span : 0;
    }
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

  std::size_t max_columns = 0;
  for (const std::string& line : document_->lines) {
    max_columns =
        std::max(max_columns, TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size_));
  }
  cached_max_visual_columns_ = max_columns;
  cached_max_visual_columns_tab_size_ = tab_size_;
  cached_max_visual_columns_revision_ = document_->layout_revision;
  return *cached_max_visual_columns_;
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

TextViewport::DecodedDocument TextViewport::DecodeDocument(std::string_view content) {
  DecodedDocument decoded;
  decoded.encoding = DetectEncoding(content);

  std::size_t crlf_count = 0;
  std::size_t lf_count = 0;
  std::size_t cr_count = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r') {
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++crlf_count;
        ++i;
      } else {
        ++cr_count;
      }
    } else if (content[i] == '\n') {
      ++lf_count;
    }
  }

  const std::size_t present_styles =
      (crlf_count > 0 ? 1 : 0) + (lf_count > 0 ? 1 : 0) + (cr_count > 0 ? 1 : 0);
  decoded.mixed_line_endings = present_styles > 1;
  if (crlf_count >= lf_count && crlf_count >= cr_count && crlf_count > 0) {
    decoded.line_ending = LineEnding::CRLF;
  } else if (lf_count >= cr_count && lf_count > 0) {
    decoded.line_ending = LineEnding::LF;
  } else if (cr_count > 0) {
    decoded.line_ending = LineEnding::CR;
  }

  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }

    decoded.lines.emplace_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }

  if (line_start <= content.size()) {
    decoded.lines.emplace_back(content.substr(line_start));
  }
  if (decoded.lines.empty()) {
    decoded.lines.push_back("");
  }
  return decoded;
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

  return IsValidUtf8(content) ? TextEncoding::UTF8 : TextEncoding::Bytes;
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

    if (!ascii_only && !IsValidUtf8(line)) {
      return TextEncoding::Bytes;
    }
  }

  return ascii_only ? TextEncoding::ASCII : TextEncoding::UTF8;
}

bool TextViewport::IsValidUtf8(std::string_view content) {
  return util::IsValidUtf8(content);
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
