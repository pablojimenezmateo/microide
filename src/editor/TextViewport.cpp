#include "editor/TextViewport.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ostream>
#include <sstream>

namespace microide::editor {

namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;
constexpr std::size_t kMaxHistoryEntries = 128;
constexpr std::size_t kVisibleLineCacheLimit = 256;
constexpr std::size_t kHighlightCacheLimit = 256;
constexpr std::size_t kLargeFileByteThreshold = 384 * 1024;
constexpr std::size_t kLargeFileLineThreshold = 4000;

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool ShouldUseLargeFileMode(std::string_view content, std::size_t line_count) {
  return content.size() >= kLargeFileByteThreshold || line_count >= kLargeFileLineThreshold;
}

std::size_t LineEndingSize(TextViewport::LineEnding line_ending) {
  switch (line_ending) {
    case TextViewport::LineEnding::CRLF:
      return 2;
    case TextViewport::LineEnding::CR:
    case TextViewport::LineEnding::LF:
    default:
      return 1;
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
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string content = buffer.str();
  const DecodedDocument decoded = DecodeDocument(content);

  document_->path = path;
  document_->lines = decoded.lines;
  document_->line_ending = decoded.line_ending;
  document_->mixed_line_endings = decoded.mixed_line_endings;
  document_->encoding = decoded.encoding;
  document_->large_file_mode = ShouldUseLargeFileMode(content, document_->lines.size());
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = false;
  document_->dirty = false;
  InvalidateLayoutCaches();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::Save() {
  EnsureDocument();
  if (document_->path.empty()) {
    return false;
  }

  std::ofstream file(document_->path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }

  const std::string newline =
      document_->line_ending == LineEnding::CRLF ? "\r\n"
      : document_->line_ending == LineEnding::CR ? "\r"
                                                 : "\n";
  for (std::size_t i = 0; i < document_->lines.size(); ++i) {
    if (i > 0) {
      file.write(newline.data(), static_cast<std::streamsize>(newline.size()));
    }
    file.write(document_->lines[i].data(),
               static_cast<std::streamsize>(document_->lines[i].size()));
  }

  if (!file.good()) {
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

  document_->path = path;
  document_->lines = decoded.lines;
  document_->line_ending = line_ending.value_or(decoded.line_ending);
  document_->mixed_line_endings = line_ending.has_value() ? false : decoded.mixed_line_endings;
  document_->encoding = decoded.encoding;
  document_->large_file_mode = ShouldUseLargeFileMode(content, document_->lines.size());
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = false;
  document_->dirty = false;
  InvalidateLayoutCaches();
  EnsureCursorVisible();
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
  document_->path.clear();
  document_->lines = SplitLines(text);
  document_->line_ending = LineEnding::LF;
  document_->mixed_line_endings = false;
  document_->encoding = DetectEncoding(text);
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = true;
  document_->dirty = false;
  document_->large_file_mode = false;
  InvalidateLayoutCaches();
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  document_->path.clear();
  document_->lines = {""};
  document_->line_ending = LineEnding::LF;
  document_->mixed_line_endings = false;
  document_->encoding = TextEncoding::ASCII;
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = false;
  document_->dirty = false;
  document_->large_file_mode = false;
  InvalidateLayoutCaches();
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
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  SaveUndoSnapshot();
  DeleteSelection();
  auto& line = document_->lines[cursor_line_];
  line.insert(line.begin() + static_cast<std::ptrdiff_t>(cursor_column_), character);
  ++cursor_column_;
  RefreshEncoding();
  RefreshLargeFileMode();
  preferred_column_ = cursor_visual_column();
  MarkDirty();
  EnsureCursorVisible();
}

void TextViewport::InsertText(std::string_view text, bool record_undo) {
  if (text.empty()) {
    return;
  }

  if (record_undo) {
    SaveUndoSnapshot();
  }
  DeleteSelection();

  std::string normalized;
  normalized.reserve(text.size());
  for (char character : text) {
    if (character != '\r') {
      normalized.push_back(character);
    }
  }

  auto inserted_lines = SplitLines(normalized);
  if (inserted_lines.empty()) {
    return;
  }

  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::string remainder = document_->lines[cursor_line_].substr(cursor_column_);
  document_->lines[cursor_line_].erase(cursor_column_);

  if (inserted_lines.size() == 1) {
    document_->lines[cursor_line_] += inserted_lines.front();
    document_->lines[cursor_line_] += remainder;
    cursor_column_ = document_->lines[cursor_line_].size() - remainder.size();
  } else {
    document_->lines[cursor_line_] += inserted_lines.front();
    const std::size_t insertion_index = cursor_line_ + 1;
    document_->lines.insert(document_->lines.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                            inserted_lines.begin() + 1, inserted_lines.end());
    cursor_line_ += inserted_lines.size() - 1;
    document_->lines[cursor_line_] += remainder;
    cursor_column_ = document_->lines[cursor_line_].size() - remainder.size();
  }

  RefreshEncoding();
  RefreshLargeFileMode();
  preferred_column_ = cursor_visual_column();
  MarkDirty();
  EnsureCursorVisible();
}

void TextViewport::InsertNewline() {
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  SaveUndoSnapshot();
  DeleteSelection();
  auto& line = document_->lines[cursor_line_];
  std::string remainder = line.substr(cursor_column_);
  line.erase(cursor_column_);
  document_->lines.insert(document_->lines.begin() + static_cast<std::ptrdiff_t>(cursor_line_ + 1),
                          remainder);
  ++cursor_line_;
  cursor_column_ = 0;
  preferred_column_ = 0;
  RefreshEncoding();
  RefreshLargeFileMode();
  MarkDirty();
  EnsureCursorVisible();
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

  SaveUndoSnapshot();
  if (DeleteSelection()) {
    return;
  }

  if (cursor_column_ > 0) {
    auto& line = document_->lines[cursor_line_];
    const std::size_t erase_start = TextLayout::PreviousTextColumn(line, cursor_column_);
    line.erase(erase_start, cursor_column_ - erase_start);
    cursor_column_ = erase_start;
    RefreshEncoding();
    RefreshLargeFileMode();
    preferred_column_ = cursor_visual_column();
    MarkDirty();
    EnsureCursorVisible();
    return;
  }

  if (cursor_line_ == 0) {
    document_->undo_stack.pop_back();
    return;
  }

  const std::size_t previous_length = document_->lines[cursor_line_ - 1].size();
  document_->lines[cursor_line_ - 1] += document_->lines[cursor_line_];
  document_->lines.erase(document_->lines.begin() + static_cast<std::ptrdiff_t>(cursor_line_));
  --cursor_line_;
  cursor_column_ = previous_length;
  RefreshEncoding();
  RefreshLargeFileMode();
  preferred_column_ = cursor_visual_column();
  MarkDirty();
  EnsureCursorVisible();
}

void TextViewport::DeleteForward() {
  if (document_->lines.empty()) {
    return;
  }

  SaveUndoSnapshot();
  if (DeleteSelection()) {
    return;
  }

  auto& line = document_->lines[cursor_line_];
  if (cursor_column_ < line.size()) {
    const std::size_t erase_end = TextLayout::NextTextColumn(line, cursor_column_);
    line.erase(cursor_column_, erase_end - cursor_column_);
    RefreshEncoding();
    RefreshLargeFileMode();
    preferred_column_ = cursor_visual_column();
    MarkDirty();
    EnsureCursorVisible();
    return;
  }

  if (cursor_line_ + 1 >= document_->lines.size()) {
    document_->undo_stack.pop_back();
    return;
  }

  line += document_->lines[cursor_line_ + 1];
  document_->lines.erase(document_->lines.begin() + static_cast<std::ptrdiff_t>(cursor_line_ + 1));
  RefreshEncoding();
  RefreshLargeFileMode();
  preferred_column_ = cursor_visual_column();
  MarkDirty();
  EnsureCursorVisible();
}

bool TextViewport::Undo() {
  if (document_->undo_stack.empty()) {
    return false;
  }

  document_->redo_stack.push_back(HistoryEntry{
      .lines = document_->lines,
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .encoding = document_->encoding,
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  });
  RestoreSnapshot(document_->undo_stack.back());
  document_->undo_stack.pop_back();
  return true;
}

bool TextViewport::Redo() {
  if (document_->redo_stack.empty()) {
    return false;
  }

  document_->undo_stack.push_back(HistoryEntry{
      .lines = document_->lines,
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .encoding = document_->encoding,
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  });
  RestoreSnapshot(document_->redo_stack.back());
  document_->redo_stack.pop_back();
  return true;
}

bool TextViewport::ReplaceRange(const SelectionRange& range,
                                std::string_view replacement,
                                bool record_undo) {
  if (document_->lines.empty()) {
    return false;
  }

  if (record_undo) {
    SaveUndoSnapshot();
  }

  selection_anchor_ = range.start;
  cursor_line_ = range.end.line;
  cursor_column_ = range.end.column;
  preferred_column_ = cursor_visual_column();
  if (!DeleteSelection()) {
    if (record_undo && !document_->undo_stack.empty()) {
      document_->undo_stack.pop_back();
    }
    return false;
  }

  InsertText(replacement, false);
  return true;
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                const std::vector<std::string>& replacement,
                                bool record_undo) {
  EnsureDocument();
  const std::size_t line_count = document_->lines.size();
  if (line_count == 0) {
    document_->lines.push_back("");
  }

  const std::size_t clamped_start = std::min(start_line, document_->lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, document_->lines.size());
  if (record_undo) {
    SaveUndoSnapshot();
  }

  std::vector<std::string> updated_lines;
  updated_lines.reserve(clamped_start + replacement.size() +
                        (document_->lines.size() - clamped_end));
  updated_lines.insert(updated_lines.end(), document_->lines.begin(),
                       document_->lines.begin() + static_cast<std::ptrdiff_t>(clamped_start));
  updated_lines.insert(updated_lines.end(), replacement.begin(), replacement.end());
  updated_lines.insert(updated_lines.end(),
                       document_->lines.begin() + static_cast<std::ptrdiff_t>(clamped_end),
                       document_->lines.end());
  if (updated_lines.empty()) {
    updated_lines.push_back("");
  }

  document_->lines = std::move(updated_lines);
  const std::size_t cursor_line = std::min(clamped_start, document_->lines.size() - 1);
  cursor_line_ = cursor_line;
  cursor_column_ = 0;
  preferred_column_ = cursor_visual_column();
  selection_anchor_.reset();
  RefreshEncoding();
  RefreshLargeFileMode();
  MarkDirty();
  EnsureCursorVisible();
  return true;
}

std::size_t TextViewport::ReplaceAll(std::string_view needle, std::string_view replacement) {
  if (needle.empty() || document_->lines.empty()) {
    return 0;
  }

  const std::string lowered_needle = ToLower(needle);
  std::size_t replacements = 0;
  bool snapshot_saved = false;

  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    std::string lowered_line = ToLower(document_->lines[line_index]);
    std::size_t offset = lowered_line.find(lowered_needle);
    while (offset != std::string::npos) {
      if (!snapshot_saved) {
        SaveUndoSnapshot();
        snapshot_saved = true;
      }
      const SelectionRange range{
          .start = TextPosition{line_index, offset},
          .end = TextPosition{line_index, offset + needle.size()},
      };
      ReplaceRange(range, replacement, false);
      ++replacements;
      lowered_line = ToLower(document_->lines[line_index]);
      offset = lowered_line.find(lowered_needle,
                                 offset + replacement.size());
    }
  }

  if (!snapshot_saved) {
    return 0;
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

void TextViewport::MarkDirty() {
  InvalidateLayoutCaches();
  document_->placeholder = false;
  document_->dirty = true;
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

void TextViewport::RefreshEncoding() {
  document_->encoding = DetectEncoding(document_->lines);
}

void TextViewport::RefreshLargeFileMode() {
  document_->large_file_mode =
      ShouldUseLargeFileMode(std::string_view{}, document_->lines.size()) ||
      SerializedByteSize() >= kLargeFileByteThreshold;
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

  const auto& start = range->start;
  const auto& end = range->end;
  if (start.line == end.line) {
    document_->lines[start.line].erase(start.column, end.column - start.column);
  } else {
    document_->lines[start.line] =
        document_->lines[start.line].substr(0, start.column) +
        document_->lines[end.line].substr(end.column);
    document_->lines.erase(document_->lines.begin() + static_cast<std::ptrdiff_t>(start.line + 1),
                           document_->lines.begin() + static_cast<std::ptrdiff_t>(end.line + 1));
  }

  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  cursor_line_ = start.line;
  cursor_column_ = start.column;
  RefreshEncoding();
  RefreshLargeFileMode();
  preferred_column_ = cursor_visual_column();
  selection_anchor_.reset();
  MarkDirty();
  EnsureCursorVisible();
  return true;
}

void TextViewport::SaveUndoSnapshot() {
  document_->redo_stack.clear();
  document_->undo_stack.push_back(HistoryEntry{
      .lines = document_->lines,
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .encoding = document_->encoding,
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  });
  if (document_->undo_stack.size() > kMaxHistoryEntries) {
    document_->undo_stack.erase(document_->undo_stack.begin());
  }
}

void TextViewport::RestoreSnapshot(const HistoryEntry& snapshot) {
  document_->lines = snapshot.lines;
  cursor_line_ = snapshot.cursor_line;
  cursor_column_ = snapshot.cursor_column;
  preferred_column_ = snapshot.preferred_column;
  scroll_line_ = snapshot.scroll_line;
  horizontal_scroll_ = snapshot.horizontal_scroll;
  selection_anchor_ = snapshot.selection_anchor;
  document_->encoding = snapshot.encoding;
  document_->placeholder = snapshot.placeholder;
  document_->dirty = snapshot.dirty;
  RefreshLargeFileMode();
  InvalidateLayoutCaches();
  EnsureCursorVisible();
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
  std::size_t i = 0;
  while (i < content.size()) {
    const unsigned char lead = static_cast<unsigned char>(content[i]);
    if (lead <= 0x7F) {
      ++i;
      continue;
    }

    auto continuation = [&](std::size_t offset) {
      return i + offset < content.size() &&
             (static_cast<unsigned char>(content[i + offset]) & 0xC0) == 0x80;
    };

    if (lead >= 0xC2 && lead <= 0xDF) {
      if (!continuation(1)) {
        return false;
      }
      i += 2;
      continue;
    }

    if (lead == 0xE0) {
      if (i + 2 >= content.size()) {
        return false;
      }
      const unsigned char second = static_cast<unsigned char>(content[i + 1]);
      if (second < 0xA0 || second > 0xBF || !continuation(2)) {
        return false;
      }
      i += 3;
      continue;
    }

    if ((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) {
      if (!continuation(1) || !continuation(2)) {
        return false;
      }
      i += 3;
      continue;
    }

    if (lead == 0xED) {
      if (i + 2 >= content.size()) {
        return false;
      }
      const unsigned char second = static_cast<unsigned char>(content[i + 1]);
      if (second < 0x80 || second > 0x9F || !continuation(2)) {
        return false;
      }
      i += 3;
      continue;
    }

    if (lead == 0xF0) {
      if (i + 3 >= content.size()) {
        return false;
      }
      const unsigned char second = static_cast<unsigned char>(content[i + 1]);
      if (second < 0x90 || second > 0xBF || !continuation(2) || !continuation(3)) {
        return false;
      }
      i += 4;
      continue;
    }

    if (lead >= 0xF1 && lead <= 0xF3) {
      if (!continuation(1) || !continuation(2) || !continuation(3)) {
        return false;
      }
      i += 4;
      continue;
    }

    if (lead == 0xF4) {
      if (i + 3 >= content.size()) {
        return false;
      }
      const unsigned char second = static_cast<unsigned char>(content[i + 1]);
      if (second < 0x80 || second > 0x8F || !continuation(2) || !continuation(3)) {
        return false;
      }
      i += 4;
      continue;
    }

    return false;
  }

  return true;
}

std::vector<std::string> TextViewport::SplitLines(const std::string& content) {
  return DecodeDocument(content).lines;
}

bool TextViewport::IsBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
}

std::size_t TextViewport::SerializedByteSize() const {
  std::size_t total_size = 0;
  for (const std::string& line : document_->lines) {
    total_size += line.size();
  }
  if (document_->lines.size() > 1) {
    total_size += (document_->lines.size() - 1) * LineEndingSize(document_->line_ending);
  }
  return total_size;
}

}  // namespace microide::editor
