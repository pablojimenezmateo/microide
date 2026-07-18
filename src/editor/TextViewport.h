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
#include "editor/HighlightPrefetch.h"
#include "editor/LanguageContractView.h"
#include "editor/FoldingModel.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextLayoutCache.h"
#include "editor/TextViewportUndoHistory.h"
#include "util/StringUtil.h"
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
    // Hanging-indent: visual columns the renderer should prepend before this
    // continuation row's content (0 for first rows of a line).
    std::size_t indent = 0;
  };

  TextViewport();
  TextViewport(const TextViewport& other);
  TextViewport& operator=(const TextViewport& other);
  TextViewport(TextViewport&& other) noexcept;
  TextViewport& operator=(TextViewport&& other) noexcept;

  bool OpenFile(const std::filesystem::path& path);
  bool Save();

  // Save-time normalization knobs. When set, `Save()` applies these transforms
  // to the in-memory line buffer (recorded as undo) before the file is
  // serialized. Defaults are off; callers should configure them from
  // `editor.save.*` settings before invoking Save().
  void SetSaveTrimTrailingWhitespace(bool enabled) { save_trim_trailing_whitespace_ = enabled; }
  void SetSaveEnsureFinalNewline(bool enabled) { save_ensure_final_newline_ = enabled; }
  // Force the on-save line ending (the `editor.line_endings` setting). nullopt
  // ("auto") keeps the file's detected ending; a value normalizes on save.
  void SetSaveLineEnding(std::optional<LineEnding> ending) { save_line_ending_override_ = ending; }
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
  // Load an already line-split buffer directly (dirty-tab session restore), moving
  // `lines` straight into the document instead of the LoadContent(SerializeLines(...))
  // round-trip that joins the lines into one string only to re-split it. Marks the
  // buffer dirty, since a restored dirty snapshot differs from the file on disk.
  void LoadLines(std::vector<std::string> lines, const std::filesystem::path& path,
                 LineEnding line_ending);
  void SetPath(const std::filesystem::path& path);
  void SetDirty(bool dirty);
  void SetPlaceholderText(std::string text);
  void SetUntitledBuffer();
  void SetViewportSize(std::size_t visible_lines, std::size_t visible_columns);
  void SetScrollLine(std::size_t scroll_line);
  void SetHorizontalScroll(std::size_t horizontal_scroll);
  // Authoritatively re-applies a previously captured view state: cursor (or
  // selection) first, then scroll LAST so the restored scroll survives even when
  // the caret is off-screen (e.g. scroll-wheel without moving the cursor).
  // Performs no trailing EnsureCursorVisible. Call this AFTER preference/indent
  // application, which internally re-runs EnsureCursorVisible and would otherwise
  // snap scroll back onto the caret.
  void ApplyRestoredViewState(std::size_t cursor_line,
                              std::size_t cursor_column,
                              std::size_t scroll_line,
                              std::size_t horizontal_scroll,
                              const std::optional<SelectionRange>& selection = std::nullopt);
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
  void Page(int direction, bool extend_selection = false);
  void InsertCharacter(char character);
  void InsertText(std::string_view text, bool record_undo = true);
  // Paste with VSCode multi-caret semantics: with N carets and clipboard text of
  // exactly N lines, insert line i at caret i (position order); otherwise insert
  // the whole text at every caret. Single-caret pastes defer to InsertText.
  bool PasteText(std::string_view text, bool record_undo = true);
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
  // Apply a precomputed, ascending-sorted set of single-line match ranges as one
  // grouped Replace-All edit, WITHOUT re-scanning/re-folding the document. Each
  // range must be confined to one line, non-empty, in range, sorted ascending by
  // (line, column), and non-overlapping (exactly what FindLiteralSearchMatches
  // produces). Returns the number of ranges applied, or std::nullopt when the
  // caller's ranges fail validation (stale/inconsistent), in which case the
  // document is left untouched and the caller can fall back to ReplaceAll.
  // (TD-2026-07-17A-028.)
  std::optional<std::size_t> ReplaceAllRanges(const std::vector<SelectionRange>& matches,
                                              std::string_view replacement);

  const std::filesystem::path& path() const { return document_->path; }
  // Cached normalized key for the per-file presentation stores. Pass this to the
  // hot-path *ByPathKey lookups to avoid re-normalizing the path every frame.
  std::string_view path_key() const { return document_->path_key; }
  const TextBuffer& lines() const { return document_->lines; }

  // On-disk identity recorded at the last load/save. `disk_signature().exists`
  // is false for untitled buffers and files that were absent when last sampled.
  const util::FileSignature& disk_signature() const { return document_->disk_signature; }
  bool HasDiskSignature() const { return document_->disk_signature.exists; }

  // Result of comparing the file on disk *now* against the signature we recorded
  // at load/last-save. Used by coordinators to avoid clobbering external edits.
  enum class DiskConflict {
    None,      // no recorded signature, or disk still matches what we last saw
    Changed,   // file exists but its mtime/size differs from our record
    Vanished,  // file was present at load/save but is now gone
    StatError,  // the stat itself failed; treat conservatively as a conflict
  };
  DiskConflict DetectDiskConflict() const;
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
  // True when the caret's current position is the result of an in-flight text
  // edit (typing, deleting) rather than a deliberate navigation move. Edits bump
  // `content_revision` and place the caret directly; navigation routes through
  // `PlacePrimaryCaret`, which re-syncs the anchor. Used to suppress occurrence
  // highlighting while a word is being typed so the highlight does not chase a
  // growing prefix.
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

  // Caret placement for one logical line, resolved against the current cursor
  // position and scroll window. Split out of VisibleLineLayout so the layout
  // itself can be served by reference from the cache (see VisibleLineLayoutRef)
  // without baking the per-call caret into the cached object.
  struct LineCaret {
    bool visible = false;
    std::size_t column = 0;
  };
  LineCaret CaretForLine(std::size_t line_index) const;

  // Reference to the cached visible-line layout (no caret fields set). Hot
  // render paths bind this to avoid copying the LayoutLine (string + 2 vectors)
  // per visible row; pair it with CaretForLine for caret drawing.
  const LayoutLine& VisibleLineLayoutRef(std::size_t line_index) const;

  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index) const;
  // Overload for the per-frame render loop: the caller resolves the caret's
  // visual row once and threads it in, so this does not recompute
  // CursorVisualRow() (an O(caret column) walk) for every visible row.
  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index,
                                     std::size_t cursor_visual_row) const;
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
  // Background highlight prefetch. These let a worker thread tokenize ahead of
  // the viewport without touching live viewport state: BuildHighlightPrefetchRequest
  // captures an immutable snapshot on the main thread, the worker turns it into a
  // HighlightPrefetchResult, and InstallPrefetchedHighlights folds that result
  // back into the cache on the main thread (dropping it if the document changed
  // underneath). The synchronous HighlightedLineTokens path remains the
  // source of truth; prefetch only pre-populates the LRU so misses are rarer.
  bool HasHighlightPrefetchGap(std::size_t start_line, std::size_t count) const;
  HighlightPrefetchRequest BuildHighlightPrefetchRequest(std::size_t start_line,
                                                         std::size_t count) const;
  void InstallPrefetchedHighlights(HighlightPrefetchResult result);
  // Off-thread checkpoint-chain backfill. A deep first paint (e.g. session
  // restore scrolled deep into a large file) needs the syntax state before a far
  // line; building it synchronously replayed from line 0 and froze the frame.
  // The synchronous path now caps its replay and, when it falls short, records a
  // backfill target. TakeHighlightCheckpointBackfillRequest hands the shell one
  // worker request (bounded line copy) that advances the chain; the worker's
  // HighlightCheckpointResult is folded back via InstallHighlightCheckpoints.
  // Returns nullopt when no backfill is pending. Convergence is chunked across
  // repaints: each install lets the next synchronous replay resume deeper.
  std::optional<HighlightCheckpointRequest> TakeHighlightCheckpointBackfillRequest() const;
  void InstallHighlightCheckpoints(const HighlightCheckpointResult& result);
  bool syntax_highlighting_enabled() const { return !document_->placeholder; }
  TextViewportCacheStats CacheStats() const;
  void ResetCacheStats() const;
  bool dirty() const { return document_->dirty; }
  bool is_placeholder() const { return document_->placeholder; }
  std::vector<TextPosition> secondary_carets() const;
  // Full secondary carets, each carrying its selection anchor (empty for a plain
  // column caret). Shaping actions (move/indent line) use this so a ranged Ctrl-D
  // secondary selection survives the transform instead of collapsing to a bare
  // caret, and so line-range resolution covers lines spanned only by an anchor
  // (A-120). secondary_carets() (positions only) stays for plain-caret callers.
  std::vector<TextViewportUndoHistory::SecondaryCaret> secondary_caret_ranges() const;
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
  // Ranged sibling of SetSecondaryCarets: rebuilds the secondary caret set where
  // each entry carries an active selection (anchor -> cursor). Used by the
  // "add cursor at next/all match" (Ctrl+D) flow so multi-caret typing replaces
  // every matched occurrence and copy aggregates them. One clamp+sort+dedupe
  // pass keeps this O(k log k) instead of looping AddSecondaryCaretWithRange.
  void SetSecondaryCaretsWithRanges(std::vector<SelectionRange> ranges);
  void ClearSecondaryCarets();
  // Places zero-width carets on every line between anchor_line and target_line
  // (inclusive) at column. Primary caret moves to target_line; other lines become
  // secondary carets. Clears any existing secondary carets and selection.
  void PlaceColumnCaretsBetweenLines(std::size_t anchor_line, std::size_t target_line,
                                     std::size_t column);
  bool has_selection() const;
  std::optional<SelectionRange> selection_range() const;
  const std::optional<AppliedEdit>& last_applied_edit() const { return last_applied_edit_; }
  // Whole-line-trimmed span of the last applied edit (see AppliedEditLineSpan).
  // Set/cleared in lockstep with last_applied_edit(); empty for multi-region edits.
  const std::optional<AppliedEditLineSpan>& last_applied_edit_line_span() const {
    return last_applied_edit_line_span_;
  }
  std::string SelectedText() const;
  // VSCode-style multi-caret copy: each caret's selection text joined by '\n' in
  // caret position order. Returns nullopt unless there are multiple carets and
  // every caret has a non-empty selection (the Ctrl-D case) — callers then fall
  // back to the single-caret SelectedText()/line-copy behavior.
  std::optional<std::string> MultiCaretSelectedText() const;
  std::string CurrentLineTextForClipboard() const;
  bool DeleteSelectedText();
  // Delete every caret's selection atomically (one undo entry). The caller must
  // guarantee every caret has a selection (pairs with MultiCaretSelectedText for
  // multi-caret cut).
  bool DeleteMultiCaretSelections(bool record_undo = true);
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

  // Stamp/clear the last-applied-edit metadata (character-level AppliedEdit + the
  // whole-line-trimmed line span) in lockstep, so consumers never see one without
  // the other. Called from every edit primitive that sets/resets the metadata.
  void SetLastAppliedEditFromEntry(const HistoryEntry& entry, bool forward) {
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, forward);
    last_applied_edit_line_span_ = TextViewportUndoHistory::BuildAppliedEditLineSpan(entry, forward);
  }
  void ClearLastAppliedEdit() {
    last_applied_edit_.reset();
    last_applied_edit_line_span_.reset();
  }

  struct DocumentState {
    std::filesystem::path path;
    // Cached NormalizedPathKey(path), recomputed only when `path` changes.
    // Lets per-frame presentation-store lookups pass a precomputed key instead
    // of re-normalizing (and re-allocating) every frame. See SetDocumentPath.
    std::string path_key;
    TextBuffer lines;
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
    // On-disk identity captured at load and after each successful save. Used to
    // detect that the file changed underneath us (external editor, VCS checkout)
    // and to recognize our own writes when the file watcher echoes them back.
    util::FileSignature disk_signature;
  };

  // WrappedRowLayout is now owned by TextLayoutCache; keep the alias so the
  // small WrappedVisualRow / VisualHit helpers below still compile.
  using WrappedRowLayout = TextLayoutCache::WrappedRow;

  void ResetState(std::vector<std::string> lines,
                  const std::filesystem::path& path,
                  LineEnding line_ending,
                  bool mixed_line_endings,
                  TextEncoding encoding,
                  bool placeholder,
                  bool dirty);
  // Large-file load fast path: reset the document by handing canonical
  // '\n'-joined `text` straight to the buffer (no split-into-lines round-trip).
  // `text` must contain no '\r'. Shares all post-reset bookkeeping with
  // ResetState via ResetMetadataAfterContent.
  void ResetStateFromText(std::string text,
                          const std::filesystem::path& path,
                          LineEnding line_ending,
                          bool mixed_line_endings,
                          TextEncoding encoding,
                          bool placeholder,
                          bool dirty);
  // Shared tail of ResetState / ResetStateFromText: applies path + metadata and
  // resets cursor/selection/undo/caches once the buffer content is in place.
  void ResetMetadataAfterContent(const std::filesystem::path& path,
                                 LineEnding line_ending,
                                 bool mixed_line_endings,
                                 TextEncoding encoding,
                                 bool placeholder,
                                 bool dirty);
  // Sets document_->path and refreshes the cached document_->path_key together
  // so the two never drift. All path assignments must go through here.
  void SetDocumentPath(const std::filesystem::path& path);
  void InvalidateLayoutCaches();
  void RefreshEncoding();
  // Incremental, upgrade-only encoding refresh for the per-edit hot path: scans
  // only the inserted lines and raises the document encoding if they introduce
  // non-ASCII / non-UTF-8 bytes. Avoids the O(document) full re-detection that
  // RefreshEncoding performs on every keystroke. Encoding is re-detected fully
  // on load/reset, so a downgrade after deleting the last non-ASCII content is
  // recovered on the next reload (matching typical editor behavior).
  void UpgradeEncodingForInsertedLines(const std::vector<std::string>& inserted_lines);
  void EnsureInitialHighlightState() const;
  void EnsureHighlightCaches() const;
  void EnsureHighlightCheckpoints() const;
  SyntaxState HighlightStateBeforeLine(std::size_t line_index) const;
  std::size_t CurrentLineLength() const;
  void ClampCursorColumn();
  void ClampScrollState();
  void EnsureCursorVisible();
  // Single choke point for primary-caret navigation. Sets the primary caret,
  // updates the preferred column (unless `keep_preferred_column`, used by vertical
  // motion which tracks a sticky column), and re-syncs the navigation anchor so the
  // caret reads as "navigated" rather than "edited" (see CaretIsFromActiveTextEdit).
  void PlacePrimaryCaret(std::size_t line, std::size_t column, bool keep_preferred_column = false);
  void BeginSelectionIfNeeded(bool extend_selection);
  bool DeleteSelection();
  ViewState CaptureViewState() const;
  void RestoreViewState(const ViewState& state);
  void PushHistoryEntry(HistoryEntry entry, CoalesceHint hint = CoalesceHint{});
  void PushHistoryEntryDirect(HistoryEntry entry);
  void FlushActiveUndoGroup();
  void ApplyHistoryEntry(const HistoryEntry& entry, bool forward);
  std::optional<HistoryEntry> BuildRangeHistoryEntry(const SelectionRange& range,
                                                     std::string_view replacement) const;
  HistoryEntry BuildLineHistoryEntry(std::size_t start_line,
                                     std::size_t end_line,
                                     const std::vector<std::string>& replacement) const;
  // The three multi-caret edit fan-outs share one pipeline (collect+sort+dedup
  // carets, capture the affected slice, reverse-walk applying one history entry
  // per caret with position remap, then commit one aggregate undo entry). Only
  // the per-caret edit differs, so they route through ApplyMultiCaretEdit.
  enum class MultiCaretEditKind { Insert, Backspace, DeleteForward };
  // `per_caret_insert`, when non-null and sized to the deduped caret set on an
  // Insert, supplies a distinct string per caret in sorted order (distribute-
  // paste); otherwise every caret gets `insert_text`.
  bool ApplyMultiCaretEdit(MultiCaretEditKind kind, std::string_view insert_text,
                           bool record_undo,
                           const std::vector<std::string>* per_caret_insert = nullptr);
  bool ApplyMultiCaretInsert(std::string_view text, bool record_undo);
  // True when any two carets' affected ranges (selection, or empty point) overlap,
  // so no multi-caret apply path can run without double-editing shared content.
  // Both ApplyMultiCaretEdit and TryMultiCaretPairInsert bail when this is true.
  bool MultiCaretSelectionsOverlap() const;
  // Soft-tab insert across all carets, sizing each caret's padding to ITS OWN next
  // tab stop (a caret at a ragged column aligns to its own stop, not the primary's).
  bool ApplyMultiCaretSoftTab(bool record_undo);
  bool ApplyMultiCaretBackspace(bool record_undo);
  bool ApplyMultiCaretDeleteForward(bool record_undo);
  // Extract the document text spanned by a normalized (start <= end) range.
  std::string TextInRange(const SelectionRange& range) const;
  bool ApplyRangeEdit(const SelectionRange& range, std::string_view replacement, bool record_undo,
                      CoalesceHint hint = CoalesceHint{});
  // Wrap `norm` (normalized, validated) by prepending `open` to its first line at
  // `norm.start.column` and appending `close` to its last line at `norm.end.column`,
  // touching only the boundary lines. Avoids materializing the whole selected text
  // and the open+inner+close transient the generic range-replace path builds
  // (A-021). `open`/`close` must not contain a line break. Records one undo entry.
  bool SurroundRangeBoundaries(const SelectionRange& norm,
                               std::string_view open,
                               std::string_view close);
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
  // Brace-split-on-newline geometry for a single caret position. Returns the
  // replacement text (`"\n" + inner_indent + "\n" + base_indent`) plus the
  // inner-indent landing column when (line, column) sits between a matching
  // auto-close opener/closer, or std::nullopt otherwise. Shared by the
  // single-caret newline path and the multi-caret insert fan-out.
  struct NewlineBraceSplit {
    std::string text;
    std::string inner_indent;
  };
  std::optional<NewlineBraceSplit> ComputeNewlineBraceSplit(std::size_t line,
                                                            std::size_t column) const;
  bool InInsertionSuppressedScope(std::size_t line, std::size_t column) const;
  bool TryMultiCaretPairInsert(char ch);
  void InvalidateDerivedCaches(InvalidationReason reason);
  void InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line);
  void InvalidateVisualColumnCache();
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        const std::vector<std::string>& inserted_lines);
  void UpdateWrappedRowsAfterEdit(std::size_t start_line,
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
  static TextEncoding DetectEncoding(LineSpan lines);
  static bool IsBefore(const TextPosition& lhs, const TextPosition& rhs);

  std::size_t fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
  std::shared_ptr<DocumentState> document_;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t preferred_column_ = 0;
  // `content_revision` as of the last navigation move. Equal to the document's
  // current `content_revision` ⇒ caret is settled at a navigated position; not
  // equal ⇒ an edit has advanced the revision since, i.e. the caret is mid-edit.
  std::uint64_t caret_navigation_content_revision_ = 0;
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
  std::optional<LineEnding> save_line_ending_override_;
  LanguageContractView lc_view_;
  std::vector<SecondaryCaret> secondary_carets_;
  // Cache for secondary_caret_positions(): mirrors `secondary_carets_.position` and is rebuilt
  // lazily when sizes differ or any element changed. Capacity persists across rebuilds.
  mutable std::vector<TextPosition> secondary_caret_positions_cache_;
  mutable TextLayoutCache layout_cache_;
  mutable std::unordered_map<std::size_t, std::vector<SyntaxTokenKind>> highlight_cache_;
  mutable std::deque<std::size_t> highlight_cache_order_;
  mutable std::optional<SyntaxState> initial_highlight_state_;
  mutable std::vector<SyntaxState> line_highlight_states_;
  // Lazy invalidation cursor: entries `[line_highlight_states_valid_through_,
  // line_highlight_states_.size())` are stale and must be ignored by readers.
  // We bump this on edits instead of resetting each entry to `SyntaxState{}`,
  // which was an O(lines - start) loop on every keystroke (round-4 Finding 4).
  // INVARIANT: this is a strictly contiguous-from-0 frontier — `[0, valid_through)`
  // is always fully populated and current. Writers therefore advance it ONLY on a
  // contiguous write (`line == valid_through`), never `line >= valid_through`: a
  // replay that resumes from a checkpoint above the frontier (possible after an
  // off-thread checkpoint backfill advances the checkpoint chain without touching
  // this vector) would otherwise jump the frontier over the still-stale gap below
  // it and falsely mark those pre-edit states valid.
  mutable std::size_t line_highlight_states_valid_through_ = 0;
  mutable std::vector<SyntaxState> highlight_checkpoints_;
  // Same lazy-invalidation pattern for the periodic checkpoint vector.
  mutable std::size_t highlight_checkpoints_valid_through_ = 0;
  // Set when a synchronous highlight-state replay was capped short of the line a
  // paint requested (deep cold jump). The shell drains this via
  // TakeHighlightCheckpointBackfillRequest to schedule off-thread chain catch-up.
  // 0 means "no backfill pending". Reset whenever the highlight caches are
  // invalidated (content/syntax revision change).
  mutable std::size_t pending_checkpoint_backfill_target_line_ = 0;
  // False when the most recent HighlightStateBeforeLine returned an APPROXIMATE
  // resume state (a deep jump whose exact state would have needed an over-cap
  // replay). HighlightedLineTokens uses this to render immediately without
  // poisoning the authoritative per-line state cache; the off-thread backfill
  // then makes a later repaint exact (InstallHighlightCheckpoints clears the
  // token cache so approximate tokens are recomputed).
  mutable bool last_highlight_state_exact_ = true;
  mutable std::uint64_t highlight_state_content_revision_ = 0;
  mutable std::uint64_t highlight_state_syntax_revision_ = 0;
  mutable std::size_t highlight_queries_ = 0;
  mutable std::size_t highlight_hits_ = 0;
  mutable std::size_t highlight_state_advances_ = 0;
  mutable std::size_t highlight_checkpoint_advances_ = 0;
  std::optional<TextPosition> selection_anchor_;
  std::optional<AppliedEdit> last_applied_edit_;
  std::optional<AppliedEditLineSpan> last_applied_edit_line_span_;
  const FoldingModel* folding_model_ = nullptr;
  TextViewportUndoHistory undo_history_;

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
#endif
};

}  // namespace microide::editor
