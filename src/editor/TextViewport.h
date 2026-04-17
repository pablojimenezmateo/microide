#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"

namespace microide::editor {

struct TextViewportCacheStats {
  std::size_t visible_line_queries = 0;
  std::size_t visible_line_hits = 0;
  std::size_t highlight_queries = 0;
  std::size_t highlight_hits = 0;
};

struct TextPosition {
  std::size_t line = 0;
  std::size_t column = 0;
};

struct SelectionRange {
  TextPosition start;
  TextPosition end;
};

class TextViewport {
 public:
  enum class LineEnding {
    LF,
    CRLF,
    CR,
  };

  enum class TextEncoding {
    ASCII,
    UTF8,
    Bytes,
  };

  TextViewport();

  bool OpenFile(const std::filesystem::path& path);
  bool Save();
  void LoadContent(std::string_view content,
                   const std::filesystem::path& path = {},
                   std::optional<LineEnding> line_ending = std::nullopt);
  void SetPath(const std::filesystem::path& path);
  void SetDirty(bool dirty);
  void SetPlaceholderText(std::string text);
  void SetUntitledBuffer();
  void SetViewportSize(std::size_t visible_lines, std::size_t visible_columns);
  void SetScrollLine(std::size_t scroll_line);
  void SetHorizontalScroll(std::size_t horizontal_scroll);
  void SetTabSize(std::size_t tab_size);
  void SetIndentWidth(std::size_t indent_width);
  void SetSoftTabs(bool soft_tabs);
  void MoveCursorVertical(int delta, bool extend_selection = false);
  void MoveCursorHorizontal(int delta, bool extend_selection = false);
  void MoveCursorLineStart(bool extend_selection = false);
  void MoveCursorLineEnd(bool extend_selection = false);
  void MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection = false);
  void MoveCursorToVisualColumn(std::size_t line,
                                std::size_t visual_column,
                                bool extend_selection = false);
  void ScrollVertical(int delta);
  void Page(int direction);
  void InsertCharacter(char character);
  void InsertText(std::string_view text, bool record_undo = true);
  void InsertNewline();
  void InsertTab();
  void Backspace();
  void DeleteForward();
  bool Undo();
  bool Redo();
  bool ReplaceRange(const SelectionRange& range,
                    std::string_view replacement,
                    bool record_undo = true);
  bool ReplaceLines(std::size_t start_line,
                    std::size_t end_line,
                    const std::vector<std::string>& replacement,
                    bool record_undo = true);
  std::size_t ReplaceAll(std::string_view needle, std::string_view replacement);

  const std::filesystem::path& path() const { return document_->path; }
  const std::vector<std::string>& lines() const { return document_->lines; }
  std::size_t cursor_line() const { return cursor_line_; }
  std::size_t cursor_column() const { return cursor_column_; }
  std::size_t cursor_visual_column() const;
  std::size_t scroll_line() const { return scroll_line_; }
  std::size_t horizontal_scroll() const { return horizontal_scroll_; }
  std::size_t visible_lines() const { return visible_lines_; }
  std::size_t visible_columns() const { return visible_columns_; }
  std::size_t line_count() const { return document_->lines.size(); }
  std::size_t tab_size() const { return tab_size_; }
  std::size_t max_visual_columns() const { return MaxVisualColumns(); }
  std::size_t indent_width() const { return indent_width_; }
  bool soft_tabs() const { return soft_tabs_; }
  LineEnding line_ending() const { return document_->line_ending; }
  bool has_mixed_line_endings() const { return document_->mixed_line_endings; }
  TextEncoding encoding() const { return document_->encoding; }
  std::string LineEndingLabel() const;
  std::string EncodingLabel() const;
  LayoutLine VisibleLineLayout(std::size_t line_index) const;
  const std::vector<SyntaxTokenKind>& HighlightedLineTokens(std::size_t line_index) const;
  bool syntax_highlighting_enabled() const { return !document_->placeholder; }
  TextViewportCacheStats CacheStats() const;
  void ResetCacheStats() const;
  bool dirty() const { return document_->dirty; }
  bool is_placeholder() const { return document_->placeholder; }
  bool has_selection() const;
  std::optional<SelectionRange> selection_range() const;
  std::string SelectedText() const;
  bool DeleteSelectedText();
  void ClearSelection();
  void SelectAll();
  void InvalidateSyntaxHighlighting();

 private:
  struct HistoryEntry {
    std::vector<std::string> lines;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    std::size_t preferred_column = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::optional<TextPosition> selection_anchor;
    TextEncoding encoding = TextEncoding::ASCII;
    bool placeholder = false;
    bool dirty = false;
  };

  struct DocumentState {
    std::filesystem::path path;
    std::vector<std::string> lines;
    std::vector<HistoryEntry> undo_stack;
    std::vector<HistoryEntry> redo_stack;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    TextEncoding encoding = TextEncoding::ASCII;
    bool placeholder = true;
    bool dirty = false;
    std::size_t layout_revision = 0;
  };

  struct DecodedDocument {
    std::vector<std::string> lines;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    TextEncoding encoding = TextEncoding::ASCII;
  };

  struct VisibleLineCacheEntry {
    std::size_t line_index = 0;
    std::size_t horizontal_scroll = 0;
    std::size_t visible_columns = 0;
    std::size_t tab_size = 0;
    std::size_t caret_text_column = 0;
    std::size_t revision = 0;
    LayoutLine layout;
  };

  struct HighlightCacheEntry {
    std::size_t line_index = 0;
    std::size_t revision = 0;
    std::vector<SyntaxTokenKind> tokens;
  };

  void MarkDirty();
  void InvalidateLayoutCaches();
  void RefreshEncoding();
  void EnsureInitialHighlightState() const;
  void EnsureHighlightStatesThrough(std::size_t line_index) const;
  std::size_t CurrentLineLength() const;
  void ClampCursorColumn();
  void ClampScrollState();
  void EnsureCursorVisible();
  void BeginSelectionIfNeeded(bool extend_selection);
  bool DeleteSelection();
  void SaveUndoSnapshot();
  void RestoreSnapshot(const HistoryEntry& snapshot);
  std::size_t MaxVisualColumns() const;
  void EnsureDocument();
  static DecodedDocument DecodeDocument(std::string_view content);
  static TextEncoding DetectEncoding(std::string_view content);
  static TextEncoding DetectEncoding(const std::vector<std::string>& lines);
  static bool IsValidUtf8(std::string_view content);
  static std::vector<std::string> SplitLines(const std::string& content);
  static bool IsBefore(const TextPosition& lhs, const TextPosition& rhs);

  std::shared_ptr<DocumentState> document_;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t preferred_column_ = 0;
  std::size_t scroll_line_ = 0;
  std::size_t horizontal_scroll_ = 0;
  std::size_t visible_lines_ = 1;
  std::size_t visible_columns_ = 80;
  std::size_t tab_size_ = 4;
  std::size_t indent_width_ = 4;
  bool soft_tabs_ = false;
  mutable std::optional<std::size_t> cached_max_visual_columns_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::size_t cached_max_visual_columns_revision_ = 0;
  mutable std::vector<VisibleLineCacheEntry> visible_line_cache_;
  mutable std::vector<HighlightCacheEntry> highlight_cache_;
  mutable std::optional<SyntaxState> initial_highlight_state_;
  mutable std::vector<SyntaxState> line_highlight_states_;
  mutable std::optional<std::size_t> highlight_state_computed_through_;
  mutable std::size_t highlight_state_revision_ = 0;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;
  mutable std::size_t highlight_queries_ = 0;
  mutable std::size_t highlight_hits_ = 0;
  std::optional<TextPosition> selection_anchor_;
};

}  // namespace microide::editor
