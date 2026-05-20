#pragma once

#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <span>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/LanguageContractView.h"
#include "editor/FoldingModel.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "editor/TextViewportUndoHistory.h"
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

class TextViewport {
 public:
  using LineEnding = util::LineEnding;

  enum class TextEncoding {
    ASCII,
    UTF8,
    Bytes,
  };

  // Cause classification for derived-cache invalidation. Callers pass the
  // reason that triggered the call; the implementation fans this out into
  // tier bumps per the table in
  // openspec/changes/split-layout-revision-tiers/design.md Decision 2:
  //   ContentEdit  → content_revision + presentation_revision
  //   SyntaxConfig → syntax_revision + presentation_revision
  //   LayoutShape  → layout_shape_revision + presentation_revision
  //   Presentation → presentation_revision only
  enum class InvalidationReason {
    ContentEdit,
    SyntaxConfig,
    LayoutShape,
    Presentation,
  };

  struct WrappedVisualRow {
    std::size_t line_index = 0;
    std::size_t visual_start = 0;
    std::size_t visual_end = 0;
  };

  TextViewport();

  bool OpenFile(const std::filesystem::path& path);
  bool Save();

  // Save-time normalization knobs. When set, `Save()` applies these transforms
  // to the in-memory line buffer (recorded as undo) before the file is
  // serialized. Defaults are off; callers should configure them from
  // `editor.save.*` settings before invoking Save().
  void SetSaveTrimTrailingWhitespace(bool enabled) { save_trim_trailing_whitespace_ = enabled; }
  void SetSaveEnsureFinalNewline(bool enabled) { save_ensure_final_newline_ = enabled; }
  bool save_trim_trailing_whitespace() const { return save_trim_trailing_whitespace_; }
  bool save_ensure_final_newline() const { return save_ensure_final_newline_; }

  // Per-tab language contract for auto-close, surround, smart indent, etc.
  // Default-constructed view disables every per-language behavior; pushing in
  // a populated view is how the workspace turns these features on.
  void SetLanguageContractView(LanguageContractView view) { lc_view_ = std::move(view); }
  const LanguageContractView& language_contract_view() const { return lc_view_; }
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
  void SetSoftWrap(bool soft_wrap);
  void SetFoldingModel(const FoldingModel* folding_model);
  bool soft_wrap() const { return soft_wrap_; }
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
  // Merge subsequent edits into one undo stack entry until `EndUndoGroup()`.
  // While a group is active, individual `PushHistoryEntry` calls are suppressed.
  void BeginUndoGroup();
  void EndUndoGroup();
  bool UndoGroupActive() const { return undo_history_.IsGroupActive(); }
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
  std::size_t cursor_visual_row() const;
  std::size_t scroll_line() const { return scroll_line_; }
  std::size_t horizontal_scroll() const { return horizontal_scroll_; }
  std::size_t visible_lines() const { return visible_lines_; }
  std::size_t visible_columns() const { return visible_columns_; }
  std::size_t line_count() const { return document_->lines.size(); }
  std::uint64_t content_revision() const {
    return document_ != nullptr ? document_->content_revision : 0;
  }
  std::uint64_t syntax_revision() const {
    return document_ != nullptr ? document_->syntax_revision : 0;
  }
  std::uint64_t layout_shape_revision() const {
    return document_ != nullptr ? document_->layout_shape_revision : 0;
  }
  std::uint64_t presentation_revision() const {
    return document_ != nullptr ? document_->presentation_revision : 0;
  }
  // Minimum line index affected by the last layout invalidation (used by the
  // folding model to preserve stable bracket folds above the edit). Reset by
  // `ConsumeFoldEditAnchorLine()` after the host reads it for a recompute.
  std::size_t ConsumeFoldEditAnchorLine();
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
  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index) const;
  WrappedVisualRow WrappedVisualRowLayout(std::size_t visual_row_index) const;
  LogicalPosition LogicalPositionForVisualHit(int visual_row, int visual_col) const;
  int VisualRowCount() const;
  std::size_t visual_line_count() const;
  std::size_t visual_scroll_line() const { return scroll_line_; }
  std::size_t folding_revision() const {
    return folding_model_ != nullptr ? folding_model_->revision() : 0;
  }
  std::size_t VisualRowLineIndex(std::size_t visual_row_index) const;
  std::size_t VisualRowForLine(std::size_t line_index) const;
  const std::vector<SyntaxTokenKind>& HighlightedLineTokens(std::size_t line_index) const;
  // Non-forcing variant: returns the cached token span if the line is in the
  // syntax-highlight LRU, otherwise an empty span. Does not trigger
  // `EnsureHighlightCaches` or `HighlightLine`. Callers that walk large line
  // ranges (e.g. the folding bracket scanner) use this to avoid thrashing the
  // LRU on lines that may be far outside the visible region.
  std::span<const SyntaxTokenKind> HighlightedLineTokensIfCached(std::size_t line_index) const;
  bool syntax_highlighting_enabled() const { return !document_->placeholder; }
  TextViewportCacheStats CacheStats() const;
  void ResetCacheStats() const;
  bool dirty() const { return document_->dirty; }
  bool is_placeholder() const { return document_->placeholder; }
  std::vector<TextPosition> secondary_carets() const;
  // Render-path accessor: returns a view into a cached vector that mirrors
  // `secondary_carets_`. The cache is rebuilt only when the positions
  // actually differ (size mismatch or any element changed), so steady-state
  // frames pay only an O(N) compare instead of allocating a fresh vector.
  // Result is valid until the next mutating operation on this viewport.
  std::span<const TextPosition> secondary_caret_positions() const;
  bool has_multiple_carets() const { return !secondary_carets_.empty(); }
  void AddSecondaryCaret(std::size_t line, std::size_t column);
  // Adds a secondary caret with an active selection (anchor → position). Used
  // by multi-caret surround and by tests; normalizes and clamps the range.
  void AddSecondaryCaretWithRange(SelectionRange range);
  void SetSecondaryCarets(std::vector<TextPosition> carets);
  void ClearSecondaryCarets();
  bool has_selection() const;
  std::optional<SelectionRange> selection_range() const;
  const std::optional<AppliedEdit>& last_applied_edit() const { return last_applied_edit_; }
  std::string SelectedText() const;
  std::string CurrentLineTextForClipboard() const;
  bool DeleteSelectedText();
  bool DeleteCurrentLine();
  void ClearSelection();
  void SelectAll();
  void SelectWordAtCursor();
  void SelectLineAtCursor();
  // Non-mutating seed span for occurrences highlight / match actions (single logical line).
  std::optional<SelectionRange> OccurrenceSeedSpanForHighlight() const;
  void InvalidateSyntaxHighlighting();

  /// Normalizes selection endpoints so callers can reuse the same invariant as `ReplaceRange`.
  static SelectionRange NormalizeRange(const SelectionRange& range);

 private:
  // SecondaryCaret, ViewState, HistoryEntry now live on TextViewportUndoHistory.
  // Type-alias them here so the historical call sites compile unchanged.
  using SecondaryCaret = TextViewportUndoHistory::SecondaryCaret;
  using ViewState = TextViewportUndoHistory::ViewState;
  using HistoryEntry = TextViewportUndoHistory::Entry;

  struct DocumentState {
    std::filesystem::path path;
    std::vector<std::string> lines;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    TextEncoding encoding = TextEncoding::ASCII;
    bool placeholder = true;
    bool dirty = false;
    // Four-tier cache-invalidation revisions. Each counter monotonically
    // increases when a mutation of its kind occurs. Derived caches key on the
    // minimum tier set they actually depend on so a theme change does not
    // invalidate wrapped-row layouts, a scroll does not invalidate the
    // highlight cache, and so on. See
    // openspec/changes/split-layout-revision-tiers/ for the full contract.
    std::uint64_t content_revision = 0;
    std::uint64_t syntax_revision = 0;
    std::uint64_t layout_shape_revision = 0;
    std::uint64_t presentation_revision = 0;
  };

  struct VisibleLineCacheKey {
    std::size_t line_index = 0;
    std::size_t horizontal_scroll = 0;
    std::size_t visible_columns = 0;
    std::size_t tab_size = 0;

    bool operator==(const VisibleLineCacheKey& o) const noexcept {
      return line_index == o.line_index && horizontal_scroll == o.horizontal_scroll &&
             visible_columns == o.visible_columns && tab_size == o.tab_size;
    }
  };

  struct WrappedRowLayout {
    std::size_t line_index = 0;
    std::size_t visual_start = 0;
    std::size_t visual_end = 0;
  };

  struct VisibleLineCacheKeyHash {
    std::size_t operator()(const VisibleLineCacheKey& k) const noexcept {
      std::size_t h = k.line_index;
      h ^= k.horizontal_scroll * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.visible_columns * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.tab_size * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
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
  void PushHistoryEntryDirect(HistoryEntry entry);
  void FlushActiveUndoGroup();
  void ApplyHistoryEntry(const HistoryEntry& entry, bool forward);
  std::optional<HistoryEntry> BuildRangeHistoryEntry(const SelectionRange& range,
                                                     std::string_view replacement) const;
  HistoryEntry BuildLineHistoryEntry(std::size_t start_line,
                                     std::size_t end_line,
                                     const std::vector<std::string>& replacement) const;
  bool ApplyMultiCaretInsert(std::string_view text, bool record_undo);
  bool ApplyMultiCaretBackspace(bool record_undo);
  bool ApplyMultiCaretDeleteForward(bool record_undo);
  bool ApplyRangeEdit(const SelectionRange& range, std::string_view replacement, bool record_undo);
  bool ApplyLineEdit(std::size_t start_line,
                     std::size_t end_line,
                     const std::vector<std::string>& replacement,
                     bool record_undo);
  std::string AutoIndentForNewline(std::size_t line, std::size_t column) const;
  std::string IndentUnit() const;
  bool TryAutoCloseInsert(char ch);
  bool TrySurroundInsert(char ch);
  bool TrySkipOverClose(char ch);
  bool MaybeDedentOnClose(char ch);
  bool TryInsertNewlineSplitBraces();
  bool InInsertionSuppressedScope(std::size_t line, std::size_t column) const;
  bool TryMultiCaretPairInsert(char ch);
  void InvalidateDerivedCaches(InvalidationReason reason);
  void InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line);
  void InvalidateVisualColumnCache();
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        const std::vector<std::string>& inserted_lines);
  std::size_t MaxVisualColumns() const;
  void EnsureHighlightCheckpoint(std::size_t checkpoint_index) const;
  void EnsureDocument();
  void EnsureWrappedRowLayouts() const;
  // Returns the row payload for `visual_row_index`. Synthesizes the result
  // inline in the trivial-layout fast path (no soft-wrap, no collapsed folds)
  // so callers do not have to index into a vector that was never built.
  WrappedRowLayout WrappedRowAt(std::size_t visual_row_index) const;
  // Number of visual rows. O(1) in either path; in trivial mode this is the
  // document line count (less hidden lines).
  std::size_t WrappedRowCount() const;
  // Document line for the row's offset table; identity in trivial mode.
  std::size_t WrappedLineRowOffset(std::size_t line_index) const;
  void AdvanceCaretVertical(TextPosition& caret, std::size_t& preferred_column, int delta) const;
  void AdvanceCaretHorizontal(TextPosition& caret, int delta) const;
  void DedupeSecondaryCaretsAgainstPrimary();
  std::size_t PreferredColumnForCaret(const TextPosition& caret) const;
  std::size_t CursorVisualRow() const;
  std::size_t CursorVisualRowForCaret(const TextPosition& caret) const;
  std::size_t ResolveSoftWrapCursorColumnForTargetRow(const TextPosition& caret,
                                                       std::size_t preferred_column,
                                                       std::size_t target_row) const;
  std::size_t ResolveSoftWrapCursorColumnForTargetRow(std::size_t target_row) const;
  static TextEncoding DetectEncoding(std::string_view content);
  static TextEncoding DetectEncoding(const std::vector<std::string>& lines);
  static std::vector<std::string> SliceLines(const std::vector<std::string>& lines,
                                             std::size_t start_line,
                                             std::size_t end_line);
  static bool IsBefore(const TextPosition& lhs, const TextPosition& rhs);

  std::size_t fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
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
  bool soft_wrap_ = false;
  bool save_trim_trailing_whitespace_ = false;
  bool save_ensure_final_newline_ = false;
  LanguageContractView lc_view_;
  std::vector<SecondaryCaret> secondary_carets_;
  // Cache for secondary_caret_positions(): mirrors `secondary_carets_.position` and is rebuilt
  // lazily when sizes differ or any element changed. Capacity persists across rebuilds.
  mutable std::vector<TextPosition> secondary_caret_positions_cache_;
  mutable std::vector<WrappedRowLayout> wrapped_row_layouts_;
  mutable std::vector<std::size_t> wrapped_line_row_offsets_;
  mutable std::size_t wrapped_row_layouts_tab_size_ = 0;
  mutable std::size_t wrapped_row_layouts_visible_columns_ = 0;
  mutable std::uint64_t wrapped_row_layouts_layout_shape_revision_ = 0;
  mutable bool wrapped_row_layouts_soft_wrap_ = false;
  mutable const FoldingModel* wrapped_row_layouts_folding_model_ = nullptr;
  mutable std::size_t wrapped_row_layouts_fold_revision_ = 0;
  // Trivial-layout fast path: when soft-wrap is off AND no fold is collapsed,
  // the visual row ↔ document line mapping is identity. In that case
  // `wrapped_row_layouts_` / `wrapped_line_row_offsets_` are left empty and
  // the readers synthesize the row data on the fly. This avoids the
  // O(line_count) vector rebuild that the previous implementation paid for
  // on every edit of a large file.
  mutable bool wrapped_row_layouts_trivial_ = false;
#ifndef NDEBUG
  mutable std::size_t wrapped_row_layout_build_count_ = 0;
#endif
  mutable std::optional<std::size_t> cached_max_visual_columns_;
  mutable std::optional<std::size_t> cached_max_visual_columns_line_index_;
  mutable std::deque<std::size_t> cached_visual_line_columns_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::uint64_t cached_max_visual_columns_content_revision_ = 0;
  mutable std::unordered_map<VisibleLineCacheKey, LayoutLine, VisibleLineCacheKeyHash>
      visible_line_cache_;
  mutable std::deque<VisibleLineCacheKey> visible_line_cache_order_;
  mutable std::unordered_map<std::size_t, std::vector<SyntaxTokenKind>> highlight_cache_;
  mutable std::deque<std::size_t> highlight_cache_order_;
  mutable std::optional<SyntaxState> initial_highlight_state_;
  mutable std::vector<SyntaxState> line_highlight_states_;
  // Lazy invalidation cursor: entries `[line_highlight_states_valid_through_,
  // line_highlight_states_.size())` are stale and must be ignored by readers.
  // We bump this on edits instead of resetting each entry to `SyntaxState{}`,
  // which was an O(lines - start) loop on every keystroke (round-4 Finding 4).
  mutable std::size_t line_highlight_states_valid_through_ = 0;
  mutable std::vector<SyntaxState> highlight_checkpoints_;
  // Same lazy-invalidation pattern for the periodic checkpoint vector.
  mutable std::size_t highlight_checkpoints_valid_through_ = 0;
  mutable std::uint64_t highlight_state_content_revision_ = 0;
  mutable std::uint64_t highlight_state_syntax_revision_ = 0;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;
  mutable std::size_t highlight_queries_ = 0;
  mutable std::size_t highlight_hits_ = 0;
  mutable std::size_t highlight_state_advances_ = 0;
  mutable std::size_t highlight_checkpoint_advances_ = 0;
  std::optional<TextPosition> selection_anchor_;
  std::optional<AppliedEdit> last_applied_edit_;
  const FoldingModel* folding_model_ = nullptr;
  TextViewportUndoHistory undo_history_;

#ifndef NDEBUG
 public:
  std::size_t WrappedRowLayoutBuildCountForDebug() const { return wrapped_row_layout_build_count_; }
#endif
};

}  // namespace microide::editor
