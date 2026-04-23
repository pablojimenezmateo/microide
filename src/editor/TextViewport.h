#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "util/StringUtil.h"

namespace microide::editor {

struct TextViewportCacheStats {
  std::size_t visible_line_queries = 0;
  std::size_t visible_line_hits = 0;
  std::size_t highlight_queries = 0;
  std::size_t highlight_hits = 0;
  std::size_t highlight_state_advances = 0;
  std::size_t highlight_checkpoint_advances = 0;
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
  using LineEnding = util::LineEnding;

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
  struct ViewState {
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    std::size_t preferred_column = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::optional<TextPosition> selection_anchor;
    bool placeholder = false;
    bool dirty = false;
  };

  struct HistoryEntry {
    std::size_t start_line = 0;
    std::vector<std::string> before_lines;
    std::vector<std::string> after_lines;
    ViewState before_state;
    ViewState after_state;
  };

  struct DocumentState {
    std::filesystem::path path;
    std::vector<std::string> lines;
    std::deque<HistoryEntry> undo_stack;
    std::deque<HistoryEntry> redo_stack;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    TextEncoding encoding = TextEncoding::ASCII;
    bool placeholder = true;
    bool dirty = false;
    std::size_t layout_revision = 0;
  };

  struct VisibleLineCacheKey {
    std::size_t line_index = 0;
    std::size_t horizontal_scroll = 0;
    std::size_t visible_columns = 0;
    std::size_t tab_size = 0;
    std::size_t caret_text_column = 0;

    bool operator==(const VisibleLineCacheKey& o) const noexcept {
      return line_index == o.line_index && horizontal_scroll == o.horizontal_scroll &&
             visible_columns == o.visible_columns && tab_size == o.tab_size &&
             caret_text_column == o.caret_text_column;
    }
  };

  struct VisibleLineCacheKeyHash {
    std::size_t operator()(const VisibleLineCacheKey& k) const noexcept {
      std::size_t h = k.line_index;
      h ^= k.horizontal_scroll * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.visible_columns * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.tab_size * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.caret_text_column * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  void ResetState(std::vector<std::string> lines,
                  const std::filesystem::path& path,
                  LineEnding line_ending,
                  bool mixed_line_endings,
                  TextEncoding encoding,
                  bool placeholder,
                  bool dirty);
  void InvalidateLayoutCaches();
  void RefreshEncoding();
  void EnsureInitialHighlightState() const;
  void EnsureHighlightCaches() const;
  void EnsureHighlightCheckpoints() const;
  SyntaxState HighlightStateBeforeLine(std::size_t line_index) const;
  std::size_t CurrentLineLength() const;
  void ClampCursorColumn();
  void ClampScrollState();
  void EnsureCursorVisible();
  void BeginSelectionIfNeeded(bool extend_selection);
  bool DeleteSelection();
  ViewState CaptureViewState() const;
  void RestoreViewState(const ViewState& state);
  void PushHistoryEntry(HistoryEntry entry);
  void ApplyHistoryEntry(const HistoryEntry& entry, bool forward);
  std::optional<HistoryEntry> BuildRangeHistoryEntry(const SelectionRange& range,
                                                     std::string_view replacement) const;
  HistoryEntry BuildLineHistoryEntry(std::size_t start_line,
                                     std::size_t end_line,
                                     const std::vector<std::string>& replacement) const;
  static HistoryEntry BuildHistoryEntryForDocumentChange(const std::vector<std::string>& before_lines,
                                                         const ViewState& before_state,
                                                         const std::vector<std::string>& after_lines,
                                                         const ViewState& after_state);
  bool ApplyRangeEdit(const SelectionRange& range, std::string_view replacement, bool record_undo);
  bool ApplyLineEdit(std::size_t start_line,
                     std::size_t end_line,
                     const std::vector<std::string>& replacement,
                     bool record_undo);
  void InvalidateDerivedCaches();
  void InvalidateVisualColumnCache();
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        const std::vector<std::string>& inserted_lines);
  std::size_t MaxVisualColumns() const;
  void EnsureDocument();
  static TextEncoding DetectEncoding(std::string_view content);
  static TextEncoding DetectEncoding(const std::vector<std::string>& lines);
  static std::vector<std::string> SliceLines(const std::vector<std::string>& lines,
                                             std::size_t start_line,
                                             std::size_t end_line);
  static SelectionRange NormalizeRange(const SelectionRange& range);
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
  mutable std::optional<std::size_t> cached_max_visual_columns_line_index_;
  mutable std::deque<std::size_t> cached_visual_line_columns_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::size_t cached_max_visual_columns_revision_ = 0;
  mutable std::unordered_map<VisibleLineCacheKey, LayoutLine, VisibleLineCacheKeyHash>
      visible_line_cache_;
  mutable std::deque<VisibleLineCacheKey> visible_line_cache_order_;
  mutable std::unordered_map<std::size_t, std::vector<SyntaxTokenKind>> highlight_cache_;
  mutable std::deque<std::size_t> highlight_cache_order_;
  mutable std::optional<SyntaxState> initial_highlight_state_;
  mutable std::vector<std::optional<SyntaxState>> line_highlight_states_;
  mutable std::vector<SyntaxState> highlight_checkpoints_;
  mutable std::size_t highlight_state_revision_ = 0;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;
  mutable std::size_t highlight_queries_ = 0;
  mutable std::size_t highlight_hits_ = 0;
  mutable std::size_t highlight_state_advances_ = 0;
  mutable std::size_t highlight_checkpoint_advances_ = 0;
  std::optional<TextPosition> selection_anchor_;
};

}  // namespace microide::editor
