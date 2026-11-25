#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <span>
#include <vector>

#include "editor/CaretNeighborhood.h"
#include "editor/ColumnSelection.h"
#include "editor/EditTypes.h"
#include "editor/HighlightPrefetch.h"
#include "editor/IndentDetect.h"
#include "editor/LanguageContractView.h"
#include "editor/LineEditSpan.h"
#include "editor/FoldingModel.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextLayoutCache.h"
#include "editor/TextViewportMemberPolicies.h"
#include "editor/TextViewportUndoHistory.h"
#include "util/StringUtil.h"
#include "project/FileWriteGate.h"
#include "util/TextFileIO.h"

namespace microide::editor {

struct TextViewportCacheStats {
  std::size_t visible_line_queries = 0;
  std::size_t visible_line_hits = 0;
  std::size_t highlight_queries = 0;
  std::size_t highlight_hits = 0;
  std::size_t highlight_state_advances = 0;
  std::size_t highlight_checkpoint_advances = 0;
};

struct TextViewportDerivedCacheBytes {
  std::size_t layout_cache = 0;
  std::size_t highlight_tokens = 0;
  std::size_t highlight_states = 0;
  std::size_t caret_caches = 0;
  std::size_t undo_history = 0;

  std::size_t total() const {
    return layout_cache + highlight_tokens + highlight_states + caret_caches + undo_history;
  }
};

class TextViewport {
 public:
  using LineEnding = util::LineEnding;

  enum class TextEncoding {
    ASCII,
    UTF8,
    Bytes,
  };

  enum class InvalidationReason {
    ContentEdit,
    SyntaxConfig,
    LayoutShape,
    Presentation,
  };

  struct ContentSplice {
    std::size_t removed = 0;
    std::size_t inserted = 0;
    std::size_t edit_column = TextLayoutCache::kNoEditColumn;
  };

  using WrappedVisualRow = TextLayoutCache::WrappedRow;

  TextViewport();
  TextViewport(const TextViewport& other) = default;
  TextViewport& operator=(const TextViewport& other) = default;
  TextViewport(TextViewport&& other) noexcept = default;
  TextViewport& operator=(TextViewport&& other) noexcept = default;

  struct ClassifiedContent {
    std::string text;
    TextEncoding encoding = TextEncoding::UTF8;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    bool utf8_bom = false;
    std::size_t raw_content_hash = 0;
    std::size_t raw_size = 0;
  };

  bool OpenFile(const std::filesystem::path& path);
  static ClassifiedContent ClassifyContent(std::string bytes);
  bool AdoptClassifiedContent(const std::filesystem::path& path, ClassifiedContent content);
  bool AdoptFileContent(const std::filesystem::path& path, std::string bytes) {
    return AdoptClassifiedContent(path, ClassifyContent(std::move(bytes)));
  }
  enum class SaveTransforms { Apply, Skip };
  [[nodiscard]] bool Save(project::FileWriteGate& gate, std::error_code* error = nullptr,
                          SaveTransforms transforms = SaveTransforms::Apply);
  void SetSaveTrimTrailingWhitespace(bool enabled) { save_trim_trailing_whitespace_ = enabled; }
  void SetSaveEnsureFinalNewline(bool enabled) { save_ensure_final_newline_ = enabled; }
  void SetSaveLineEnding(std::optional<LineEnding> ending) { save_line_ending_override_ = ending; }
  bool save_trim_trailing_whitespace() const { return save_trim_trailing_whitespace_; }
  bool save_ensure_final_newline() const { return save_ensure_final_newline_; }

  void SetLanguageContractView(std::shared_ptr<const LanguageContractView> view) {
    lc_view_ = std::move(view);
  }
  void SetLanguageContractView(LanguageContractView view) {
    lc_view_ = std::make_shared<const LanguageContractView>(std::move(view));
  }
  const LanguageContractView& language_contract_view() const {
    return lc_view_ != nullptr ? *lc_view_ : kNoLanguageContractView;
  }

  const std::string& language_id() const;
  void SetLanguageOverride(std::string language);
  const std::string& language_override() const;
  void LoadContent(std::string_view content,
                   const std::filesystem::path& path = {},
                   std::optional<LineEnding> line_ending = std::nullopt);
  void LoadLines(std::vector<std::string> lines, const std::filesystem::path& path,
                 LineEnding line_ending);
  void SetPath(const std::filesystem::path& path);
  void RebindToPath(const std::filesystem::path& path,
                    const util::FileSignature& signature = {});
  void SetDirty(bool dirty);
  void SetPlaceholderText(std::string text);
  void SetUntitledBuffer();
  void SetViewportSize(std::size_t visible_lines, std::size_t visible_columns);
  void RevealCaret();
  void SetScrollLine(std::size_t scroll_line);
  void ReconcileCaretsWithDocument() { SyncCaretsIfDocumentChangedElsewhere(); }
  void TraceCaretBounds(const char* where) const;
  std::string CaretBoundsFault() const;
  bool CaretsLagDocument() const {
    return document_ != nullptr && carets_synced_content_revision_ != document_->content_revision;
  }
  void SetHorizontalScroll(std::size_t horizontal_scroll);
  void ApplyRestoredViewState(std::size_t cursor_line,
                              std::size_t cursor_column,
                              std::size_t scroll_line,
                              std::size_t horizontal_scroll,
                              const std::optional<SelectionRange>& selection = std::nullopt);
  void ReloadPreservingViewState(std::string_view text);
  bool ApplyFormattedText(std::string_view text);
  void SetTabSize(std::size_t tab_size);
  void SetIndentWidth(std::size_t indent_width);
  void SetSoftTabs(bool soft_tabs);
  void SetOwnIndent(const IndentDetection& own) { own_indent_ = own; }
  const IndentDetection& own_indent() const { return own_indent_; }
  void SetSoftWrap(bool soft_wrap);
  void SetFoldingModel(const FoldingModel* folding_model);
  bool soft_wrap() const { return soft_wrap_; }
  void MoveCursorVertical(int delta, bool extend_selection = false);
  void MoveCursorHorizontal(int delta, bool extend_selection = false);
  void MoveCursorWord(int delta, bool extend_selection = false);
  void DeleteWord(int direction);
  void MoveCursorLineStart(bool extend_selection = false);
  void MoveCursorLineEnd(bool extend_selection = false);
  void MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection = false);
  void JumpCursorTo(std::size_t line, std::size_t column, bool extend_selection = false);
  void MoveCursorToVisualColumn(std::size_t line,
                                std::size_t visual_column,
                                bool extend_selection = false);
  void ScrollVertical(int delta);
  void CenterLine(std::size_t line);
  void Page(int direction, bool extend_selection = false);
  void InsertCharacter(char character);
  void InsertText(std::string_view text, bool record_undo = true);
  bool PasteText(std::string_view text, bool record_undo = true);
  void InsertNewline();
  void InsertTab();
  void Backspace();
  void DeleteForward();
  bool Undo();
  bool Redo();
  bool TopUndoEntryIsColumnScopedForTesting() const;
  void BeginUndoGroup();
  void EndUndoGroup();
  bool UndoGroupActive() const { return document_->undo_history.IsGroupActive(); }
  bool ReplaceRange(const SelectionRange& range,
                    std::string_view replacement,
                    bool record_undo = true);
  bool ReplaceLines(std::size_t start_line,
                    std::size_t end_line,
                    std::vector<std::string> replacement,
                    bool record_undo = true);
  bool ReplaceLines(std::size_t start_line,
                    std::size_t end_line,
                    LineBlob replacement,
                    bool record_undo = true);
  std::size_t ReplaceAll(std::string_view needle, std::string_view replacement);
  std::optional<std::size_t> ReplaceAllRanges(const std::vector<SelectionRange>& matches,
                                              std::string_view replacement);

  const std::filesystem::path& path() const { return document_->path; }
  std::string_view path_key() const { return document_->path_key; }
  const std::filesystem::path& real_path() const { return document_->real_path; }
  void RefreshRealPath() {
    if (document_ != nullptr) {
      SetDocumentPath(document_->path);
    }
  }
  const TextBuffer& lines() const { return document_->lines; }

  const util::FileSignature& disk_signature() const { return document_->disk_signature; }
  bool HasDiskSignature() const { return document_->disk_signature.exists; }
  bool SharesDocumentWith(const TextViewport& other) const {
    return document_ == other.document_;
  }
  const void* document_identity() const { return document_.get(); }

  enum class DiskConflict {
    None,
    Changed,
    Vanished,
    StatError,
  };
  DiskConflict DetectDiskConflict() const;
  bool SaveWouldOverwriteUnseenDisk() const {
    const DiskConflict conflict = DetectDiskConflict();
    return conflict == DiskConflict::Changed || conflict == DiskConflict::StatError;
  }
  bool DiskContentUnchanged(const util::FileSignature& current) const;
  static constexpr std::uintmax_t kMaxConfirmBytes = 8u << 20;
  bool CouldConfirmDiskContent(const util::FileSignature& current) const;
  bool ConfirmDiskContentUnchanged(const util::FileSignature& current,
                                   std::uint64_t disk_content_hash) const;
  bool HoldsSameFileContentAs(const TextViewport& other) const;
  void AdoptDiskBaselineOf(const TextViewport& other);
  bool RevertToDiskContent(const TextViewport& disk);
  void NoteDiskChangedUnderEdits();
  bool DirtyWithACleanPosition() const;
  std::size_t cursor_line() const { return cursor_line_; }
  std::size_t cursor_column() const { return cursor_column_; }
  std::size_t cursor_visual_column() const;
  std::size_t VisualColumnAt(std::size_t line, std::size_t column) const;
  std::size_t TextColumnAtVisualColumn(std::size_t line, std::size_t visual_column) const;
  LineLayoutFacts CachedLineFacts(std::size_t line) const;
  bool PlainAsciiThroughColumn(std::size_t line, std::size_t column) const;
  std::size_t PlainAsciiPrefixEnd(std::size_t line, std::size_t probe) const;
  std::size_t cursor_visual_row() const;
  std::size_t scroll_line() const { return scroll_line_; }
  std::size_t horizontal_scroll() const { return horizontal_scroll_; }
  std::size_t visible_lines() const { return visible_lines_; }
  void SetStickyBandRows(std::size_t rows) { sticky_band_rows_ = rows; }
  std::size_t sticky_band_rows() const { return sticky_band_rows_; }
  std::size_t visible_columns() const { return visible_columns_; }
  std::size_t line_count() const { return document_->lines.size(); }
  std::uint64_t content_revision() const {
    return document_ != nullptr ? document_->content_revision : 0;
  }
  bool CaretIsFromActiveTextEdit() const {
    return document_ != nullptr &&
           caret_navigation_content_revision_ != document_->content_revision;
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
  LineEditSpan ConsumeFoldEditSpan();
  std::size_t tab_size() const { return tab_size_; }
  std::size_t max_visual_columns() const { return MaxVisualColumns(); }
  void PrebuildLineWidths() const {
    (void)MaxVisualColumns();
    layout_cache_.ArmLineWidthParking();
  }
  std::size_t indent_width() const { return indent_width_; }
  bool soft_tabs() const { return soft_tabs_; }
  LineEnding line_ending() const { return document_->line_ending; }
  bool SetLineEnding(LineEnding ending);
  bool has_mixed_line_endings() const { return document_->mixed_line_endings; }
  TextEncoding encoding() const { return document_->encoding; }
  bool has_utf8_bom() const { return document_->utf8_bom; }
  void SetUtf8Bom(bool has_bom) { document_->utf8_bom = has_bom; }
  std::string SerializeDocumentText(std::optional<LineEnding> line_ending = std::nullopt) const;
  std::string SerializeText() const {
    return util::SerializeLinesStreaming(LineSpan(document_->lines), document_->line_ending);
  }
  std::string LineEndingLabel() const;
  std::string EncodingLabel() const;
  LayoutLine VisibleLineLayout(std::size_t line_index) const;

  struct LineCaret {
    bool visible = false;
    std::size_t column = 0;
  };
  LineCaret CaretForLine(std::size_t line_index) const;

  const LayoutLine& VisibleLineLayoutRef(std::size_t line_index) const;
  bool VisibleLineLruIsConsistentForTesting() const {
    return layout_cache_.VisibleLineLruIsConsistentForTesting();
  }
  std::string VerifyDerivedCachesForTesting() const;

  std::size_t LogicalLineVisualWidth(std::size_t line_index) const;

  const LayoutLine& VisibleWrappedRowLayoutRef(std::size_t visual_row_index) const;
  LineCaret CaretForWrappedRow(std::size_t visual_row_index,
                               std::size_t cursor_visual_row) const;

  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index) const;
  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index,
                                     std::size_t cursor_visual_row) const;
  WrappedVisualRow WrappedVisualRowLayout(std::size_t visual_row_index) const;
  LogicalPosition LogicalPositionForVisualHit(int visual_row, int visual_col) const;
  struct VisualHit {
    LogicalPosition position;
    WrapRowAffinity affinity = WrapRowAffinity::kNextRow;
  };
  VisualHit ResolveVisualHit(int visual_row, int visual_col) const;
  void MoveCursorToVisualHit(int visual_row, int visual_col, bool extend_selection = false);
  int VisualRowCount() const;
  std::size_t visual_line_count() const;
  std::size_t visual_scroll_line() const { return scroll_line_; }
  std::size_t folding_revision() const {
    return folding_model_ != nullptr ? folding_model_->revision() : 0;
  }
  const FoldingModel* folding_model() const { return folding_model_; }
  TextViewportDerivedCacheBytes DerivedCacheBytes() const;

  std::size_t VisualRowLineIndex(std::size_t visual_row_index) const;
  std::size_t VisualRowForLine(std::size_t line_index) const;
  const std::vector<SyntaxTokenKind>& HighlightedLineTokens(std::size_t line_index) const;
  std::span<const SyntaxTokenKind> HighlightedLineTokensIfCached(std::size_t line_index) const;
  std::optional<std::span<const SyntaxTokenKind>> CurrentHighlightedLineTokens(std::size_t line_index) const;
  void SettleHighlightAfterLastEdit() const;
  void AppendCachedHighlightedLines(std::vector<std::size_t>& out) const;
  bool HasHighlightPrefetchGap(std::size_t start_line, std::size_t count) const;
  static std::size_t HighlightCacheCapacity();
  HighlightPrefetchRequest BuildHighlightPrefetchRequest(std::size_t start_line,
                                                         std::size_t count) const;
  void InstallPrefetchedHighlights(HighlightPrefetchResult result);
  std::optional<HighlightCheckpointRequest> TakeHighlightCheckpointBackfillRequest() const;
  void InstallHighlightCheckpoints(const HighlightCheckpointResult& result);
  bool syntax_highlighting_enabled() const { return !document_->placeholder; }
  TextViewportCacheStats CacheStats() const;
  void ResetCacheStats() const;
  bool dirty() const { return document_->dirty; }
  bool is_placeholder() const { return document_->placeholder; }

  void SetReadOnly(bool read_only) { read_only_ = read_only; }
  [[nodiscard]] bool read_only() const { return read_only_; }
  [[nodiscard]] std::uint64_t refused_edits() const { return refused_edits_; }
  std::vector<TextPosition> secondary_carets() const;
  std::vector<TextViewportUndoHistory::SecondaryCaret> secondary_caret_ranges() const;
  std::span<const TextViewportUndoHistory::SecondaryCaret> secondary_caret_range_view() const {
    return secondary_carets_;
  }
  std::span<const TextPosition> secondary_caret_positions() const;
  bool has_multiple_carets() const { return !secondary_carets_.empty(); }
  void AddSecondaryCaret(std::size_t line, std::size_t column);
  bool RemoveSecondaryCaretAt(TextPosition position);
  void AddSecondaryCaretWithRange(SelectionRange range);
  void SetSecondaryCarets(std::vector<TextPosition> carets);
  bool AddCaretVertical(int delta);
  bool AddCaretsAtSelectedLineEnds();
  bool ExpandSelectionToWholeLines();
  [[nodiscard]] bool AnySelectionContains(const TextPosition& position) const;
  void SetSecondaryCaretsWithRanges(std::span<const SelectionRange> ranges);
  void ClearSecondaryCarets();
  void PruneCoincidentSecondaryCarets();
  void PlaceColumnCaretsBetweenLines(std::size_t anchor_line, std::size_t target_line,
                                     std::size_t column);
  void SetBoxSelection(TextPosition anchor, TextPosition caret);
  void SetBoxSelectionVisual(std::size_t anchor_line, std::size_t anchor_visual_column,
                             std::size_t caret_line, std::size_t caret_visual_column);
  std::size_t BoxCornerVisualColumn(TextPosition corner) const;
  void BoxColumnsOnLine(std::size_t line, std::size_t anchor_visual_column,
                        std::size_t caret_visual_column, std::size_t* anchor_column,
                        std::size_t* caret_column);
  ColumnSelectionState column_selection() const {
    if (column_selection_.active && document_ != nullptr &&
        column_selection_.document_revision != document_->content_revision) {
      return ColumnSelectionState{};
    }
    return column_selection_;
  }
  void SetColumnSelection(const ColumnSelectionState& selection) {
    column_selection_ = selection;
    column_selection_.document_revision = document_ != nullptr ? document_->content_revision : 0;
  }
  void ClearColumnSelection() { column_selection_ = ColumnSelectionState{}; }
  std::size_t MaxVisualWidthInSpan(std::size_t lo, std::size_t hi) const;
  bool has_selection() const;
  std::optional<SelectionRange> selection_range() const;
  const std::optional<TextPosition>& selection_anchor() const { return selection_anchor_; }
  const std::optional<AppliedEdit>& last_applied_edit() const { return last_applied_edit_; }
  bool LastEditChangedCarriedSyntaxState(std::size_t line) const;
  const AppliedEdit* LatestAppliedEdit() const {
    return last_applied_edit_.has_value() && document_ != nullptr &&
                   last_applied_edit_revision_ == document_->content_revision
               ? &*last_applied_edit_
               : nullptr;
  }
  const AppliedEdit* AppliedEditFromRevision(std::uint64_t base_revision) const {
    if (!last_applied_edit_.has_value() || document_ == nullptr ||
        last_applied_edit_base_revision_ != base_revision ||
        last_applied_edit_revision_ != document_->content_revision) {
      return nullptr;
    }
    return &*last_applied_edit_;
  }
  const std::optional<AppliedEditLineSpan>& last_applied_edit_line_span() const {
    return last_applied_edit_line_span_;
  }
  std::string SelectedText() const;
  std::string TextInRange(const SelectionRange& range) const;
  std::optional<std::string> MultiCaretSelectedText() const;
  std::string CurrentLineTextForClipboard() const;
  void AppendCaretLineVerbLines(std::vector<std::size_t>* out) const;
  std::optional<std::string> MultiCaretLineTextForClipboard() const;
  std::size_t CollapsedFoldEndAt(std::size_t line) const;
  std::size_t VisibleLineAfter(std::size_t line) const;
  std::size_t VisibleLineBefore(std::size_t line) const;
  bool DeleteSelectedText();
  bool DeleteMultiCaretSelections(bool record_undo = true);
  bool DeleteCurrentLine();
  void ClearSelection();
  void SelectAll();
  void SelectWordAtCursor();
  void SelectWordOrRunAtCursor();
  std::optional<SelectionRange> WordRangeAt(TextPosition position) const;
  std::optional<SelectionRange> WordSelectionRangeAt(TextPosition position) const;
  SelectionRange LineRangeAt(std::size_t line_index) const;
  void SelectLineAtCursor();
  bool ExpandSelection();
  bool ShrinkSelection();
  std::optional<SelectionRange> OccurrenceSeedSpanForHighlight() const;
  void InvalidateSyntaxHighlighting();

  static SelectionRange NormalizeRange(const SelectionRange& range);

  void TraceBufferIo(std::string_view event) const;
  void TraceBufferIo(std::string_view event, std::string_view bytes) const;

 private:
  using SecondaryCaret = TextViewportUndoHistory::SecondaryCaret;
  using ViewState = TextViewportUndoHistory::ViewState;
  using HistoryEntry = TextViewportUndoHistory::Entry;

  void SetLastAppliedEditFromEntry(const HistoryEntry& entry, bool forward, std::uint64_t base_revision) {
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, forward);
    last_applied_edit_line_span_ = TextViewportUndoHistory::BuildAppliedEditLineSpan(entry, forward);
    last_applied_edit_base_revision_ = base_revision;
    last_applied_edit_revision_ = document_ != nullptr ? document_->content_revision : 0;
  }
  void ClearLastAppliedEdit() {
    last_applied_edit_.reset();
    last_applied_edit_line_span_.reset();
    last_applied_edit_base_revision_ = 0;
    last_applied_edit_revision_ = 0;
  }

  static std::uint64_t NextDocumentRevisionBase();
  struct DocumentState {
    std::filesystem::path path;
    std::string language_override;
    std::filesystem::path real_path;
    std::string path_key;
    TextBuffer lines;
    LineEnding line_ending = LineEnding::LF;
    bool mixed_line_endings = false;
    TextEncoding encoding = TextEncoding::ASCII;
    bool utf8_bom = false;
    bool placeholder = true;
    bool dirty = false;
    std::uint64_t content_revision = NextDocumentRevisionBase();
    std::uint64_t head_content_revision = 0;
    std::uint64_t syntax_revision = 0;
    std::uint64_t layout_shape_revision = 0;
    std::uint64_t presentation_revision = 0;
    util::FileSignature disk_signature;
    TextViewportUndoHistory undo_history;
  };

  using WrappedRowLayout = TextLayoutCache::WrappedRow;

  void ResetState(std::vector<std::string> lines,
                  const std::filesystem::path& path,
                  LineEnding line_ending,
                  bool mixed_line_endings,
                  TextEncoding encoding,
                  bool placeholder,
                  bool dirty);
  void ResetStateFromText(std::string text,
                          const std::filesystem::path& path,
                          LineEnding line_ending,
                          bool mixed_line_endings,
                          TextEncoding encoding,
                          bool placeholder,
                          bool dirty);
  void ResetMetadataAfterContent(const std::filesystem::path& path,
                                 LineEnding line_ending,
                                 bool mixed_line_endings,
                                 TextEncoding encoding,
                                 bool placeholder,
                                 bool dirty);
  void SetDocumentPath(const std::filesystem::path& path);
  void InvalidateLayoutCaches();
  void RefreshEncoding();
  void UpgradeEncodingForInsertedLines(const std::vector<std::string>& inserted_lines);
  void UpgradeEncodingForInsertedLines(const LineBlob& inserted_lines);
  void UpgradeEncodingFromDelta(TextEncoding delta);
  void UpgradeEncodingForInsertedText(std::string_view text);
  void EnsureInitialHighlightState() const;
  void EnsureHighlightCaches() const;
  struct HighlightCacheEntry {
    std::vector<SyntaxTokenKind> tokens;
    std::uint64_t generation = 0;
  };
  void DropHighlightTokenCache() const;
  void StaleHighlightTokensFrom(std::size_t start_line) const;
  const HighlightCacheEntry* CurrentHighlightCacheEntry(std::size_t line_index) const;
  SyntaxState HighlightStateBeforeLine(std::size_t line_index) const;
  void SelectOnCursorLine(std::size_t start_column, std::size_t end_column);
  void SelectRangeForExpansion(const SelectionRange& range);
  void ClampScrollState();
  void EnsureCursorVisible();
  void PlacePrimaryCaret(std::size_t line, std::size_t column, bool keep_preferred_column = false,
                         WrapRowAffinity affinity = WrapRowAffinity::kNextRow);
  void BeginSelectionIfNeeded(bool extend_selection);
  bool DeleteSelection();
  ViewState CaptureViewState() const;
  ViewState CaptureViewStateForGroupedEntry() const;

  class SuppressEntryCaretCapture {
   public:
    explicit SuppressEntryCaretCapture(TextViewport& viewport) : viewport_(viewport) {
      previous_ = viewport_.suppress_entry_caret_capture_;
      viewport_.suppress_entry_caret_capture_ = true;
    }
    SuppressEntryCaretCapture(const SuppressEntryCaretCapture&) = delete;
    SuppressEntryCaretCapture& operator=(const SuppressEntryCaretCapture&) = delete;
    ~SuppressEntryCaretCapture() { viewport_.suppress_entry_caret_capture_ = previous_; }

   private:
    TextViewport& viewport_;
    bool previous_ = false;
  };
  ViewState CaptureViewStateImpl(bool with_secondary_carets) const;
  void RestoreViewState(const ViewState& state);
  bool ApplyHistoryStep(bool redo);
  void PushHistoryEntry(HistoryEntry entry, CoalesceHint hint = CoalesceHint{});
  void CommitLineRangeEdit(std::size_t first_changed_line,
                           LineBlob before_changed_lines,
                           LineBlob after_changed_lines,
                           const ViewState& before_state);
  void CarryCaretsThroughReplacements(const std::vector<SelectionRange>& matches,
                                      std::string_view replacement);
  void FlushActiveUndoGroup();
  void ApplyHistoryEntry(const HistoryEntry& entry, bool forward);
  std::optional<HistoryEntry> BuildRangeHistoryEntry(const SelectionRange& range,
                                                     std::string_view replacement) const;
  std::optional<HistoryEntry> BuildInlineHistoryEntry(std::size_t line,
                                                      std::size_t start_column,
                                                      std::size_t end_column,
                                                      std::string_view replacement) const;
  void WidenInlineEntryToLines(HistoryEntry& entry) const;
  HistoryEntry BuildLineHistoryEntry(std::size_t start_line,
                                     std::size_t end_line,
                                     LineBlob replacement) const;
  enum class MultiCaretEditKind {
    Insert,
    SoftTab,
    Backspace,
    DeleteForward,
    DeleteWordBackward,
    DeleteWordForward,
  };
  bool ApplyMultiCaretEdit(MultiCaretEditKind kind, std::string_view insert_text,
                           bool record_undo,
                           const std::vector<std::string>* per_caret_insert = nullptr);
  bool ApplyMultiCaretInsert(std::string_view text, bool record_undo);
  static CoalesceHint MultiCaretCoalesceHint(MultiCaretEditKind kind, std::string_view text,
                                             bool per_caret);
  bool MultiCaretSelectionsOverlap() const;
  bool ApplyMultiCaretSoftTab(bool record_undo);
  bool ApplyMultiCaretBackspace(bool record_undo);
  bool ApplyMultiCaretDeleteForward(bool record_undo);
  bool ApplyRangeEdit(const SelectionRange& range, std::string_view replacement, bool record_undo,
                      CoalesceHint hint = CoalesceHint{});
  void CollapseSelectionAfterNoOpEdit(const SelectionRange& range, std::string_view replacement);
  bool SurroundRangeBoundaries(const SelectionRange& norm,
                               std::string_view open,
                               std::string_view close);
  bool ApplyLineEdit(std::size_t start_line,
                     std::size_t end_line,
                     LineBlob replacement,
                     bool record_undo);
  std::string AutoIndentForNewline(std::size_t line, std::size_t column) const;
  std::string IndentUnit() const;
  std::optional<std::string> DocCommentContinuation(std::string_view line, std::size_t column,
                                                    const std::string& base_indent) const;
  CaretNeighborhood ReadCaretNeighborhood(std::size_t line, std::size_t column) const {
    return editor::ReadCaretNeighborhood(document_->lines, line, column);
  }

  bool TryAutoCloseInsert(char ch);
  bool TrySurroundInsert(char ch);
  bool TrySkipOverClose(char ch);
  bool CaretSitsInsideAutoClosedPair(std::size_t line, std::size_t column) const;
  std::optional<std::size_t> IndentStopBackspaceStart(std::size_t line, std::size_t column) const;
  bool MaybeDedentOnClose(char ch);
  bool TryInsertNewlineSplitBraces();
  struct NewlineBraceSplit {
    std::string text;
    std::string inner_indent;
  };
  std::optional<NewlineBraceSplit> ComputeNewlineBraceSplit(std::size_t line,
                                                            std::size_t column) const;
  bool InInsertionSuppressedScope(std::size_t line, std::size_t column) const;
  bool TryMultiCaretPairInsert(char ch);
  void InvalidateDerivedCaches(InvalidationReason reason);
  void InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line,
                               std::optional<ContentSplice> splice = std::nullopt);
  void InvalidateVisualColumnCache();
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        std::size_t inserted_count,
                                        InlineLineSplice splice = {});
  void UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                  std::size_t removed_count,
                                  std::size_t inserted_count,
                                  std::size_t edit_column = TextLayoutCache::kNoEditColumn);
  std::size_t MaxVisualColumns() const;
  void EnsureHighlightCheckpoint(std::size_t checkpoint_index) const;
  void EnsureDocument();
  void ClampCaretSetToDocument();
  TextPosition ClampedToDocument(TextPosition position) const;

  void SyncCaretsWithSharedDocument();
  void SyncCaretsIfDocumentChangedElsewhere() {
    if (document_ != nullptr &&
        carets_synced_content_revision_ != document_->content_revision) {
      SyncCaretsWithSharedDocument();
    }
  }
  void EnsureWrappedRowLayouts() const;
  WrappedRowLayout WrappedRowAt(std::size_t visual_row_index) const;
  std::size_t WrappedRowCount() const;
  std::size_t LineVisualWidthFromWrappedRows(std::size_t line_index) const;
  enum class VerticalEdgePolicy { kClamp, kToRowEdge };
  void AdvanceCaretVertical(TextPosition& caret, std::size_t& preferred_column,
                            WrapRowAffinity& affinity, int delta,
                            VerticalEdgePolicy edge_policy) const;
  void AdvanceCaretHorizontal(TextPosition& caret, int delta) const;
  TextPosition WordTargetForCaret(const TextPosition& caret,
                                  int delta,
                                  bool for_deletion) const;
  void DedupeSecondaryCaretsAgainstPrimary();
  void MergeOverlappingCaretRanges();
  std::size_t PreferredColumnForCaret(const TextPosition& caret,
                                      WrapRowAffinity affinity = WrapRowAffinity::kNextRow) const;
  std::size_t CursorVisualRow() const;
  std::size_t CursorVisualRowForCaret(const TextPosition& caret,
                                      WrapRowAffinity affinity = WrapRowAffinity::kNextRow) const;
  std::size_t ResolveSoftWrapCursorColumnForTargetRow(std::size_t preferred_column,
                                                      std::size_t target_row) const;
  WrapRowAffinity AffinityForRowLanding(std::size_t row_index, std::size_t visual_column) const;
  struct ViewLineBounds {
    std::size_t first = 0;
    std::size_t last = 0;
    bool last_is_wrap_point = false;
  };
  ViewLineBounds ViewLineBoundsForCaret(const TextPosition& caret,
                                        WrapRowAffinity affinity) const;
  std::size_t FirstNonWhitespaceColumnInView(std::size_t line,
                                             const ViewLineBounds& bounds) const;
  WrapRowAffinity EffectiveCaretAffinity() const {
    return caret_wrap_affinity_ == WrapRowAffinity::kPreviousRow &&
                   caret_wrap_affinity_position_.line == cursor_line_ &&
                   caret_wrap_affinity_position_.column == cursor_column_
               ? WrapRowAffinity::kPreviousRow
               : WrapRowAffinity::kNextRow;
  }
  static TextEncoding DetectEncoding(std::string_view content);
  static TextEncoding DetectEncoding(LineSpan lines);
  static bool StripUtf8Bom(std::string& content, TextEncoding& encoding);
  static bool IsBefore(const TextPosition& lhs, const TextPosition& rhs);

  LineEditSpan fold_edit_span_;
  void RecordOpenedContentHash(std::size_t raw_content_hash, std::size_t raw_size);

  detail::SharedOnMove<DocumentState> document_;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t preferred_column_ = 0;
  WrapRowAffinity caret_wrap_affinity_ = WrapRowAffinity::kNextRow;
  TextPosition caret_wrap_affinity_position_;
  std::uint64_t caret_navigation_content_revision_ = 0;
  std::uint64_t carets_synced_content_revision_ = 0;
  std::size_t scroll_line_ = 0;
  std::size_t horizontal_scroll_ = 0;
  std::size_t visible_lines_ = 1;
  std::size_t sticky_band_rows_ = 0;
  static constexpr std::size_t kNoPendingCenter = static_cast<std::size_t>(-1);
  std::size_t pending_center_line_ = kNoPendingCenter;
  std::size_t visible_columns_ = 80;
  std::size_t tab_size_ = 4;
  std::size_t indent_width_ = 4;
  bool soft_tabs_ = false;
  IndentDetection own_indent_;
  bool soft_wrap_ = false;
  bool save_trim_trailing_whitespace_ = false;
  bool save_ensure_final_newline_ = false;
  std::optional<LineEnding> save_line_ending_override_;
  bool read_only_ = false;
  std::uint64_t refused_edits_ = 0;
  std::shared_ptr<const LanguageContractView> lc_view_;
  mutable std::string language_id_;
  mutable const void* language_id_document_ = nullptr;
  mutable std::filesystem::path language_id_path_;
  mutable std::uint64_t language_id_content_revision_ = 0;
  mutable std::size_t language_id_registry_revision_ = 0;
  mutable bool language_id_valid_ = false;
  mutable bool language_id_content_sensitive_ = false;
  std::vector<SecondaryCaret> secondary_carets_;
  ColumnSelectionState column_selection_;
  mutable std::vector<TextPosition> secondary_caret_positions_cache_;
  detail::ScratchBuffer<SelectionRange> box_ranges_scratch_;
  detail::ScratchBuffer<SecondaryCaret> secondary_caret_candidates_scratch_;
  struct CaretMergeEntry {
    TextPosition start;
    TextPosition end;
    bool caret_at_end = true;
    std::size_t source = 0;
  };
  detail::ScratchBuffer<CaretMergeEntry> caret_merge_scratch_;
  mutable detail::TransferredLayoutCache layout_cache_;
  mutable std::unordered_map<std::size_t, HighlightCacheEntry> highlight_cache_;
  mutable std::uint64_t highlight_cache_generation_ = 1;
  static constexpr std::uint64_t kStaleByLastEdit = std::uint64_t{1} << 63;
  mutable std::deque<std::size_t> highlight_cache_order_;
  mutable std::optional<SyntaxState> initial_highlight_state_;
  mutable std::vector<SyntaxState> line_highlight_states_;
  mutable std::size_t line_highlight_states_valid_through_ = 0;
  struct EditCarry {
    std::size_t first_line = 0;
    std::size_t line = 0;
    SyntaxState before;
    std::size_t states_valid_through_before = 0;
    std::size_t checkpoints_valid_through_before = 0;
    std::uint64_t content_revision = 0;
    bool settled = false;
  };
  mutable std::optional<EditCarry> edit_carry_before_;
  void StoreLineHighlightState(std::size_t line, const SyntaxState& state) const;
  mutable std::vector<SyntaxState> highlight_checkpoints_;
  mutable std::size_t highlight_checkpoints_valid_through_ = 0;
  mutable std::size_t pending_checkpoint_backfill_target_line_ = 0;
  mutable bool last_highlight_state_exact_ = true;
  mutable std::uint64_t highlight_state_content_revision_ = 0;
  mutable std::uint64_t highlight_state_syntax_revision_ = 0;
  mutable std::size_t highlight_queries_ = 0;
  mutable std::size_t highlight_hits_ = 0;
  mutable std::size_t highlight_state_advances_ = 0;
  mutable std::size_t highlight_checkpoint_advances_ = 0;
  std::optional<TextPosition> selection_anchor_;
  mutable bool caret_bounds_reported_ = false;
  std::vector<SelectionRange> expand_selection_history_;
  std::optional<AppliedEdit> last_applied_edit_;
  std::uint64_t last_applied_edit_base_revision_ = 0;
  std::uint64_t last_applied_edit_revision_ = 0;
  std::optional<AppliedEditLineSpan> last_applied_edit_line_span_;
  std::uint64_t undo_group_base_revision_ = 0;
  detail::NotTransferred<bool> suppress_entry_caret_capture_;
  detail::NotTransferred<const FoldingModel*> folding_model_;

#ifndef NDEBUG
 public:
  std::size_t WrappedRowLayoutBuildCountForDebug() const {
    return layout_cache_.wrapped_row_layout_build_count_for_debug();
  }
  std::size_t WrappedRowIncrementalInplaceCountForDebug() const {
    return layout_cache_.wrapped_row_incremental_inplace_count_for_debug();
  }
  std::size_t WrappedRowIncrementalSpliceCountForDebug() const {
    return layout_cache_.wrapped_row_incremental_splice_count_for_debug();
  }
  std::size_t VisualColumnIncrementalInplaceCountForDebug() const {
    return layout_cache_.visual_column_incremental_inplace_count_for_debug();
  }
  std::size_t VisualColumnSpliceDerivedCountForDebug() const {
    return layout_cache_.visual_column_splice_derived_count_for_debug();
  }
#endif
};

}
