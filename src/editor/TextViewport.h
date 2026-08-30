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
#include "editor/LanguageContractView.h"
#include "editor/LineEditSpan.h"
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

// What one open tab costs to KEEP open, broken down by which cache is holding
// it (TD-2026-08-06-142). Every field is heap bytes held by container capacity;
// the document text itself is deliberately absent, because it is the tab's
// content and not something a cap could reclaim.
//
// The breakdown, not just the total, is the point. All five of these are
// individually bounded BY THE DOCUMENT and none of them is bounded across tabs,
// so choosing a ceiling means knowing which one dominates — and until this
// existed, "how much does an open tab cost to keep open?" had no answer at all.
struct TextViewportDerivedCacheBytes {
  // Per-line visual width table, the wrapped-row table, the 256-entry
  // visible-line LRU and their scratch.
  std::size_t layout_cache = 0;
  // Per-line syntax token LRU.
  std::size_t highlight_tokens = 0;
  // Per-line highlighter state, sized to the document, plus the periodic
  // checkpoint chain.
  std::size_t highlight_states = 0;
  // Multi-caret mirrors and the box-selection scratch.
  std::size_t caret_caches = 0;
  // Undo/redo history. Not a derived cache — it cannot be recomputed — but it
  // is per-tab retained memory that grows with use, so a ceiling that ignored
  // it would be measuring the wrong thing.
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

  // Exact extent of one content edit: lines [start, start + removed) were
  // replaced by `inserted` lines. Reported only by the edit path that applies a
  // history entry, which is the one place the counts are known.
  struct ContentSplice {
    std::size_t removed = 0;
    std::size_t inserted = 0;
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
  //
  // The view is held by shared_ptr because every tab of the same language wants
  // the *same* view: the workspace builds one per (language, toggle set) and
  // hands the same instance to all of them, so a settings change no longer
  // deep-copies four vectors and three strings into every open tab
  // (TD-2026-08-03-110). Null means "no contract" and reads back as a static
  // default-constructed view.
  void SetLanguageContractView(std::shared_ptr<const LanguageContractView> view) {
    lc_view_ = std::move(view);
  }
  // Convenience overload for callers that own a one-off view (tests, and any
  // caller with no shared table behind it). Allocates; the shared overload does
  // not.
  void SetLanguageContractView(LanguageContractView view) {
    lc_view_ = std::make_shared<const LanguageContractView>(std::move(view));
  }
  // Inline (not out-of-line in the .cpp): the per-keystroke language-behavior
  // paths consult this several times per typed character, and a non-LTO build
  // would otherwise pay a cross-TU call for a pointer deref.
  const LanguageContractView& language_contract_view() const {
    return lc_view_ != nullptr ? *lc_view_ : kNoLanguageContractView;
  }

  // Memoized filetype id for this buffer ("c++", "python", … ; empty when the
  // syntax registry has no match).
  //
  // Detection is cheap per call but not free: it materializes the
  // signature-scan head of the buffer into a vector<std::string>, runs the
  // filename / header / signature regexes, and returns an owned std::string.
  // Callers ask constantly and almost always about an unchanged buffer -- every
  // prepared frame (fold refresh, status bar), every LSP/assist request, and
  // once per open tab on every settings change.
  //
  // The answer is a pure function of (document identity, content revision, path,
  // syntax-registry revision), so the memo keys on exactly those and is
  // self-invalidating. This supersedes the per-caller `runtime_syntax::
  // FiletypeMemo` instances that the status bar and the per-tab fold state each
  // used to carry.
  //
  // The reference is into the memo, so it stays valid only until the next
  // resolve *on this viewport*. Callers that hold it across an edit, a path
  // change, or an unrelated call that might re-resolve should copy it.
  const std::string& language_id() const;
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
  // Replaces the buffer with `text` and puts the view back exactly where it was:
  // same viewport size, path and line ending, same cursor/selection, same scroll.
  //
  // The save path (format-on-save, save participants) and the virtual-document
  // reload path each hand-rolled this, and the two had drifted: one restored the
  // selection AFTER the scroll, so MoveCursorTo's EnsureCursorVisible overwrote the
  // scroll it had just restored. Select-all then save-with-formatter jumped the
  // view to the end of the file. Restoring view state is this class's business, so
  // it lives here and both callers are one line.
  void ReloadPreservingViewState(std::string_view text);
  void SetTabSize(std::size_t tab_size);
  void SetIndentWidth(std::size_t indent_width);
  void SetSoftTabs(bool soft_tabs);
  void SetSoftWrap(bool soft_wrap);
  void SetFoldingModel(const FoldingModel* folding_model);
  bool soft_wrap() const { return soft_wrap_; }
  void MoveCursorVertical(int delta, bool extend_selection = false);
  void MoveCursorHorizontal(int delta, bool extend_selection = false);
  // Word-granular horizontal motion: Ctrl+Left (`delta < 0`, VS Code
  // `cursorWordStartLeft`) and Ctrl+Right (`delta > 0`, `cursorWordEndRight`),
  // with Shift extending. Crosses the line boundary in both directions the way
  // the character form does. Applies to every caret; the boundary rule is
  // `editor/WordBoundary.h`, shared with the single-line surfaces.
  void MoveCursorWord(int delta, bool extend_selection = false);
  // Word-granular deletion: Ctrl+Backspace (`direction < 0`) and Ctrl+Delete
  // (`direction > 0`). Deletes the selection instead when there is one, joins
  // with the neighbouring line at a line edge, and takes an indent run on its
  // own (see `DeleteWordBoundaryLeft`). Applies to every caret.
  void DeleteWord(int direction);
  void MoveCursorLineStart(bool extend_selection = false);
  void MoveCursorLineEnd(bool extend_selection = false);
  // Reposition the caret, LEAVING any secondary carets in place. This is the
  // primitive: its internal callers (ShapingActions rebuilding a caret set,
  // SnippetEngine placing a tabstop, the drag collapse-then-extend idiom) depend
  // on the secondaries surviving. User-facing jumps want JumpCursorTo below.
  void MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection = false);
  // A user-facing JUMP to a location — goto-line, find-next, jump-to-bracket,
  // go-to-definition, a problems/search-result reveal, a stack-frame click, a
  // plugin's open-at-line. Collapses to a single caret first, then moves, which
  // is what VS Code does for every one of these: a multi-caret set is anchored
  // to where the user was working, and a jump elsewhere strands it off-screen
  // where its next edit is invisible (TD-2026-08-13-203). Prefer this over
  // ClearSecondaryCarets() + MoveCursorTo(): the open-coded pair is how these
  // call sites diverged in the first place.
  void JumpCursorTo(std::size_t line, std::size_t column, bool extend_selection = false);
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
  // Test seam: is the top undo entry the column-scoped form?
  //
  // An edit that stays inside one line must record {column, removed, inserted},
  // not the whole affected line before and after. The two forms are
  // indistinguishable from the buffer's content, and the line form costs a
  // multiple of the LINE per keystroke -- which on a file with no line breaks in
  // it is a multiple of the document (TD-2026-08-05-131). Exposed so a test can
  // pin the routing instead of trusting it, in the same spirit as
  // TextBuffer::materialized_line_count().
  bool TopUndoEntryIsColumnScopedForTesting() const;
  // Merge subsequent edits into one undo stack entry until `EndUndoGroup()`.
  // While a group is active, individual `PushHistoryEntry` calls are suppressed.
  void BeginUndoGroup();
  void EndUndoGroup();
  bool UndoGroupActive() const { return undo_history_.IsGroupActive(); }
  bool ReplaceRange(const SelectionRange& range,
                    std::string_view replacement,
                    bool record_undo = true);
  // `replacement` is taken BY VALUE and moved into the history entry's
  // after-image. A line op over a multi-caret span builds one replacement vector
  // covering every line between the outermost carets, and copying it here was one
  // full copy of that span per edit.
  bool ReplaceLines(std::size_t start_line,
                    std::size_t end_line,
                    std::vector<std::string> replacement,
                    bool record_undo = true);
  // Blob form: the shaping ops (toggle comment, sort, move line, indent) build
  // their replacement straight into one buffer plus a line-start table, so an
  // n-line op costs a constant number of allocations rather than n
  // (TD-2026-08-11-182).
  bool ReplaceLines(std::size_t start_line,
                    std::size_t end_line,
                    LineBlob replacement,
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
  // Visual column of byte `column` on `line` in the live buffer, and its inverse.
  //
  // Both consult the layout cache's per-line facts first: on a plain-ASCII line
  // the two coordinates coincide, so the answer is O(1) rather than a walk to the
  // column. Every caret path in the editor goes through this pair, which is what
  // keeps a caret a megabyte into one line from costing a megabyte a keystroke.
  std::size_t VisualColumnAt(std::size_t line, std::size_t column) const;
  std::size_t TextColumnAtVisualColumn(std::size_t line, std::size_t visual_column) const;
  // What the layout cache currently knows about `line`, or an unknown value when
  // its width table does not describe this document.
  LineLayoutFacts CachedLineFacts(std::size_t line) const;
  // First byte of `line` that is not plain single-cell ASCII, searched in
  // [0, probe) and returning `probe` when there is none — so byte offset IS
  // visual column everywhere below the result. Memoized per line and content
  // revision by the layout cache, which is what makes it safe to ask per rendered
  // row (see TextLayoutCache::PlainAsciiPrefixEnd).
  std::size_t PlainAsciiPrefixEnd(std::size_t line, std::size_t probe) const;
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
  // The line range that has changed since the folding model last looked, as a
  // common-prefix/common-suffix window (see LineEditSpan). The model resyncs its
  // per-line indent and bracket caches across it instead of rescanning the
  // document. Reading it resets the accumulator, so there is exactly one
  // consumer: the fold refresh.
  LineEditSpan ConsumeFoldEditSpan();
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

  // Full visual width of a logical line. Under soft wrap this is read off the
  // wrapped-row table (a line's last row ends at its width) rather than built as
  // a layout, so a caller that only needs the width -- end-of-line decorations
  // anchor past it -- does not build a whole-line LayoutLine per decorated line
  // per frame, and does not take a second reference out of the visible-line cache
  // while it is already holding one.
  std::size_t LogicalLineVisualWidth(std::size_t line_index) const;

  // Reference to the cached layout of one soft-wrapped row -- the wrap-path
  // sibling of VisibleLineLayoutRef, with the same contract: no caret fields set
  // (pair it with CaretForWrappedRow), valid until the next call that can evict
  // it. Bind this in the render loop; the by-value forms below copy a string and
  // two vectors per row.
  const LayoutLine& VisibleWrappedRowLayoutRef(std::size_t visual_row_index) const;
  // Caret for one wrapped row. `cursor_visual_row` is threaded in by the caller
  // so a frame resolves it once instead of per row.
  LineCaret CaretForWrappedRow(std::size_t visual_row_index,
                               std::size_t cursor_visual_row) const;

  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index) const;
  // Overload for the per-frame render loop: the caller resolves the caret's
  // visual row once and threads it in, so this does not recompute
  // CursorVisualRow() (an O(caret column) walk) for every visible row.
  LayoutLine VisibleWrappedRowLayout(std::size_t visual_row_index,
                                     std::size_t cursor_visual_row) const;
  WrappedVisualRow WrappedVisualRowLayout(std::size_t visual_row_index) const;
  LogicalPosition LogicalPositionForVisualHit(int visual_row, int visual_col) const;
  // Hit-test result including which wrapped row the hit belongs to when it lands
  // on a wrap boundary (see WrapRowAffinity).
  struct VisualHit {
    LogicalPosition position;
    WrapRowAffinity affinity = WrapRowAffinity::kNextRow;
  };
  VisualHit ResolveVisualHit(int visual_row, int visual_col) const;
  // Hit-test + placement in one step, for the mouse paths. Doing it here is what
  // lets a click PAST the last glyph of a soft-wrapped row keep the caret on that
  // row: the resolved position is the wrap boundary, which the plain
  // MoveCursorTo(line, column) path would render at the start of the row below
  // (VS Code keeps it at the end of the clicked row). Identical to
  // MoveCursorTo(LogicalPositionForVisualHit(...)) in every non-wrapped case.
  void MoveCursorToVisualHit(int visual_row, int visual_col, bool extend_selection = false);
  int VisualRowCount() const;
  std::size_t visual_line_count() const;
  std::size_t visual_scroll_line() const { return scroll_line_; }
  std::size_t folding_revision() const {
    return folding_model_ != nullptr ? folding_model_->revision() : 0;
  }
  // Per-tab retained-memory breakdown. Walks container capacities; not called
  // on any production path (see TextViewportDerivedCacheBytes). The folding
  // model is deliberately NOT included: it is owned per tab but reachable
  // through a bare pointer here, and the caller that owns both is the one that
  // can attribute it without double-counting a shared model.
  TextViewportDerivedCacheBytes DerivedCacheBytes() const;

  std::size_t VisualRowLineIndex(std::size_t visual_row_index) const;
  std::size_t VisualRowForLine(std::size_t line_index) const;
  const std::vector<SyntaxTokenKind>& HighlightedLineTokens(std::size_t line_index) const;
  // Non-forcing variant: returns the cached token span if the line is in the
  // syntax-highlight LRU, otherwise an empty span. Does not trigger
  // `EnsureHighlightCaches` or `HighlightLine`. Callers that walk large line
  // ranges (e.g. the folding bracket scanner) use this to avoid thrashing the
  // LRU on lines that may be far outside the visible region.
  std::span<const SyntaxTokenKind> HighlightedLineTokensIfCached(std::size_t line_index) const;
  // Appends, sorted, the line indices whose highlight tokens are currently
  // cached (at most kHighlightCacheLimit of them). The fold bracket scan uses
  // this to apply string/comment suppression by walking a cursor through the
  // set, instead of probing the hash map once per line it scans -- the probe
  // was the per-line cost left after the byte scan itself got cheap.
  void AppendCachedHighlightedLines(std::vector<std::size_t>& out) const;
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
  // Non-owning sibling of the above, for callers that only READ the set. The
  // owning form copies the whole vector, which a shaping op pays once per
  // keystroke purely to loop over it. Valid until the next mutating operation on
  // this viewport — snapshot with secondary_caret_ranges() if the set is about
  // to be edited underneath you.
  std::span<const TextViewportUndoHistory::SecondaryCaret> secondary_caret_range_view() const {
    return secondary_carets_;
  }
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
  // Takes a span, not a vector: SetBoxSelection rebuilds the whole caret set on
  // every keystroke of a held column-select gesture, so an owning parameter there
  // costs one growing allocation per keystroke. Callers keep their buffer and its
  // capacity persists.
  void SetSecondaryCaretsWithRanges(std::span<const SelectionRange> ranges);
  void ClearSecondaryCarets();
  // Places zero-width carets on every line between anchor_line and target_line
  // (inclusive) at column. Primary caret moves to target_line; other lines become
  // secondary carets. Clears any existing secondary carets and selection.
  // Degenerate box selection: delegates to SetBoxSelection with both corners on
  // `column`.
  void PlaceColumnCaretsBetweenLines(std::size_t anchor_line, std::size_t target_line,
                                     std::size_t column);
  // Rectangular (column/box) selection between two corners. Every line in the row
  // span gets a per-line selection from `anchor.column` to `caret.column`, clamped
  // to that line's length (lines shorter than both columns collapse to a zero-width
  // caret at end-of-line, matching VSCode column selection). The `caret` line holds
  // the primary caret+selection; other lines become ranged secondary carets. Clears
  // any existing secondary carets and selection. The caret span is capped so a drag
  // across a huge file cannot allocate one caret per line.
  void SetBoxSelection(TextPosition anchor, TextPosition caret);
  // Keyboard column selection (Ctrl+Shift+Alt+Arrow). Lives on the viewport rather
  // than in shell interaction state so it is naturally per-tab and survives a tab
  // switch, like the secondary carets it drives. Two positions and a bool: no
  // allocation, so this does not reopen the "TextViewport must not own copied
  // state" perf rule that applies to the language-contract view.
  const ColumnSelectionState& column_selection() const { return column_selection_; }
  void SetColumnSelection(const ColumnSelectionState& selection) { column_selection_ = selection; }
  void ClearColumnSelection() { column_selection_ = ColumnSelectionState{}; }
  // Longest line in [lo, hi], used to bound the virtual column when a column
  // selection extends right. O(span), and the span is already capped by
  // SetBoxSelection's caret limit.
  std::size_t MaxLineLengthInSpan(std::size_t lo, std::size_t hi) const;
  bool has_selection() const;
  std::optional<SelectionRange> selection_range() const;
  const std::optional<AppliedEdit>& last_applied_edit() const { return last_applied_edit_; }
  // Whole-line-trimmed span of the last applied edit (see AppliedEditLineSpan).
  // Set/cleared in lockstep with last_applied_edit(); empty for multi-region edits.
  const std::optional<AppliedEditLineSpan>& last_applied_edit_line_span() const {
    return last_applied_edit_line_span_;
  }
  std::string SelectedText() const;
  // The text of an arbitrary range. Public because a drag-and-drop move captures
  // its source at press time and applies the edit at release, so it cannot read
  // "whatever is selected now" (editor/TextDragDrop.h).
  std::string TextInRange(const SelectionRange& range) const;
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
  // The identifier run under `position`, or nullopt when it is not on one. Shared
  // with the double-click-drag path, which needs the range for a position that is
  // not the caret; SelectWordAtCursor is this plus "put the caret on it".
  std::optional<SelectionRange> WordRangeAt(TextPosition position) const;
  // The whole of `line_index`, end-exclusive at the next line's start so a
  // multi-line line-granular selection joins up across the newlines.
  SelectionRange LineRangeAt(std::size_t line_index) const;
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
    // Bumped by the subset of content edits that touch the document's HEAD --
    // the first `runtime_syntax::kFiletypeDetectHeadLines` lines, which is all a
    // content-sensitive filetype detection ever reads. A memo over such a
    // detection keys on this instead of `content_revision`, so typing at line
    // 5,000 of a `.h` file does not re-run four signature regexes over 64 lines
    // on every keystroke.
    std::uint64_t head_content_revision = 0;
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
  void UpgradeEncodingForInsertedLines(const LineBlob& inserted_lines);
  void UpgradeEncodingFromDelta(TextEncoding delta);
  // Column-scoped counterpart: an in-line splice can only raise the document's
  // classification through the bytes it actually spliced in. Every other byte on
  // the line was scanned when it first entered the document, and the line form's
  // re-scan of the whole rebuilt line is O(line) -- on a file with no line breaks
  // in it, O(document) per keystroke.
  void UpgradeEncodingForInsertedText(std::string_view text);
  void EnsureInitialHighlightState() const;
  void EnsureHighlightCaches() const;
  // One line's cached tokens plus the generation they were computed in. See
  // highlight_cache_ for why staleness is stamped rather than erased.
  struct HighlightCacheEntry {
    std::vector<SyntaxTokenKind> tokens;
    std::uint64_t generation = 0;
  };
  // Invalidate every per-line token cache entry without freeing it. See
  // highlight_cache_generation_.
  void DropHighlightTokenCache() const;
  // Invalidate the entries at or after `start_line`, keeping the ones below it —
  // the shape a content edit needs, since the syntax state chains forward and
  // only lines from the edit on can have changed. Also without freeing.
  void StaleHighlightTokensFrom(std::size_t start_line) const;
  // The entry for `line_index` if it was computed in the current generation,
  // else nullptr. The single place staleness is decided.
  const HighlightCacheEntry* CurrentHighlightCacheEntry(std::size_t line_index) const;
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
  // `affinity` is the wrap-boundary tiebreaker for the placed caret; it defaults
  // to kNextRow so every ordinary placement RESETS a sticky previous-row bit and
  // only the two paths that deliberately land on a boundary (vertical motion,
  // hit-testing) pass kPreviousRow.
  void PlacePrimaryCaret(std::size_t line, std::size_t column, bool keep_preferred_column = false,
                         WrapRowAffinity affinity = WrapRowAffinity::kNextRow);
  void BeginSelectionIfNeeded(bool extend_selection);
  bool DeleteSelection();
  ViewState CaptureViewState() const;
  // CaptureViewState for an entry that is about to be recorded as a CHILD of an
  // undo group, which is where the secondary-caret vector inside it is dead
  // weight: FinishActiveGroup overwrites the aggregate's before/after state with
  // the frame's own captures, and the disjoint extra parts carry no state at all.
  //
  // A multi-region edit builds three of these per region per keystroke, so on
  // `editor_shaping_multi_caret` (32 carets) that was 96 copies of a 31-caret
  // vector per key press, every one of them discarded. Outside a group it is
  // exactly CaptureViewState — the state is the entry's own and must be complete.
  ViewState CaptureViewStateForGroupedEntry() const;
  ViewState CaptureViewStateImpl(bool with_secondary_carets) const;
  void RestoreViewState(const ViewState& state);
  // Undo and Redo are the same walk in opposite directions: flush any open
  // group, check the stack, pop, stamp the view state on the side being moved
  // away from, apply, record the applied edit, push onto the other stack. They
  // were written out twice; a fix to one would have missed the other.
  bool ApplyHistoryStep(bool redo);
  void PushHistoryEntry(HistoryEntry entry, CoalesceHint hint = CoalesceHint{});
  // Swap a contiguous run of lines for its replacement and do everything that
  // has to follow: mark dirty, drop redo, re-sniff the encoding, drop the
  // layout caches, keep the caret on screen, and push the undo entry. Both
  // replace-all paths need exactly this sequence, and a copy that forgets one
  // step fails quietly — a missed InvalidateLayoutCaches paints stale rows, a
  // missed ClearRedo leaves a redo stack that replays against text that no
  // longer exists. Consumes both line vectors into the undo entry.
  void CommitLineRangeEdit(std::size_t first_changed_line,
                           LineBlob before_changed_lines,
                           LineBlob after_changed_lines,
                           const ViewState& before_state);
  void FlushActiveUndoGroup();
  void ApplyHistoryEntry(const HistoryEntry& entry, bool forward);
  std::optional<HistoryEntry> BuildRangeHistoryEntry(const SelectionRange& range,
                                                     std::string_view replacement) const;
  // Column-scoped entry for a one-line-in / one-line-out edit: `replacement`
  // carries no line break and the range stays on `line`. Reads only the bytes it
  // replaces, so its cost is the size of the delta rather than the size of the
  // line. nullopt for an exact no-op.
  std::optional<HistoryEntry> BuildInlineHistoryEntry(std::size_t line,
                                                      std::size_t start_column,
                                                      std::size_t end_column,
                                                      std::string_view replacement) const;
  // Rewrite a just-applied inline entry into the line-vector form, for the one
  // consumer that cannot read the column form (undo-group aggregation). Costs one
  // copy of the affected line; see PushHistoryEntry.
  void WidenInlineEntryToLines(HistoryEntry& entry) const;
  HistoryEntry BuildLineHistoryEntry(std::size_t start_line,
                                     std::size_t end_line,
                                     LineBlob replacement) const;
  // The three multi-caret edit fan-outs share one pipeline (collect+sort+dedup
  // carets, capture the affected slice, reverse-walk applying one history entry
  // per caret with position remap, then commit one aggregate undo entry). Only
  // the per-caret edit differs, so they route through ApplyMultiCaretEdit.
  enum class MultiCaretEditKind {
    Insert,
    Backspace,
    DeleteForward,
    DeleteWordBackward,
    DeleteWordForward,
  };
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
                     LineBlob replacement,
                     bool record_undo);
  std::string AutoIndentForNewline(std::size_t line, std::size_t column) const;
  std::string IndentUnit() const;
  // Caret-local bytes for the pair/indent heuristics, read without materializing
  // the line. See editor/CaretNeighborhood.h for why that matters.
  CaretNeighborhood ReadCaretNeighborhood(std::size_t line, std::size_t column) const {
    return editor::ReadCaretNeighborhood(document_->lines, line, column);
  }

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
  // `splice` reports the exact line range an edit replaced, for the derived
  // caches that can resync incrementally instead of rebuilding. std::nullopt --
  // the default -- means the extent was not reported, which degrades to "nothing
  // from `start_line` on is reusable"; only ApplyHistoryEntry knows the splice.
  void InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line,
                               std::optional<ContentSplice> splice = std::nullopt);
  void InvalidateVisualColumnCache();
  // `splice` describes an edit confined to one line by the text it inserted;
  // with it the width table updates without reading the line at all. Only
  // ApplyHistoryEntry has that, and only for a column-scoped entry.
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        std::size_t inserted_count,
                                        InlineLineSplice splice = {});
  void UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                  std::size_t removed_count,
                                  std::size_t inserted_count);
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
  // Full visual width of a logical line, read off the wrapped-row table: a line's
  // LAST row ends exactly at its width. Lets a wrapped row be built without
  // measuring the whole line it belongs to.
  std::size_t LineVisualWidthFromWrappedRows(std::size_t line_index) const;
  void AdvanceCaretVertical(TextPosition& caret, std::size_t& preferred_column,
                            WrapRowAffinity& affinity, int delta) const;
  void AdvanceCaretHorizontal(TextPosition& caret, int delta) const;
  // Where a word-granular step from `caret` lands. `for_deletion` picks the
  // whitespace-heuristic boundary (Ctrl+Backspace eats an indent run whole)
  // rather than the motion boundary; the two differ only in front of a run of
  // two or more spaces.
  TextPosition WordTargetForCaret(const TextPosition& caret,
                                  int delta,
                                  bool for_deletion) const;
  void DedupeSecondaryCaretsAgainstPrimary();
  std::size_t PreferredColumnForCaret(const TextPosition& caret,
                                      WrapRowAffinity affinity = WrapRowAffinity::kNextRow) const;
  std::size_t CursorVisualRow() const;
  std::size_t CursorVisualRowForCaret(const TextPosition& caret,
                                      WrapRowAffinity affinity = WrapRowAffinity::kNextRow) const;
  // Absolute visual column the caret should take on `target_row`, given a
  // preferred column measured in ON-SCREEN cells (hanging indent included).
  std::size_t ResolveSoftWrapCursorColumnForTargetRow(std::size_t preferred_column,
                                                      std::size_t target_row) const;
  // kPreviousRow when `visual_column` sits exactly on `row`'s trailing wrap
  // boundary, i.e. the caret belongs at the END of that row rather than at the
  // start of the next one. kNextRow everywhere else, including the last row of a
  // logical line (whose end is the end of the line, not a wrap point).
  WrapRowAffinity AffinityForRowLanding(std::size_t row_index, std::size_t visual_column) const;
  // Text-column bounds of the VIEW line a caret sits on: the wrapped row under
  // soft wrap, the whole logical line otherwise. `[first, last]` are text
  // columns; `last_is_wrap_point` says the row ends at a wrap boundary rather
  // than at the end of the logical line, which is what an End landing there
  // needs in order to render at the row's trailing edge (WrapRowAffinity).
  //
  // Home/End are view-line verbs in VS Code (`cursorHome`/`cursorEnd`), with
  // `cursorLineStart`/`cursorLineEnd` as the separate logical-line pair
  // (TD-2026-08-12-188).
  struct ViewLineBounds {
    std::size_t first = 0;
    std::size_t last = 0;
    bool last_is_wrap_point = false;
  };
  ViewLineBounds ViewLineBoundsForCaret(const TextPosition& caret,
                                        WrapRowAffinity affinity) const;
  // First non-whitespace text column at or after `bounds.first`, or
  // `bounds.first` when the view line is blank. The target of the first Home
  // press; a second press falls back to `bounds.first`, as VS Code does.
  std::size_t FirstNonWhitespaceColumnInView(std::size_t line,
                                             const ViewLineBounds& bounds) const;
  // The primary caret's affinity, honoured only while the caret is still exactly
  // where the affinity was set. Every path that moves the caret without going
  // through PlacePrimaryCaret (edit application, undo restore, language
  // behaviours) therefore drops it instead of leaving a stale bit behind.
  WrapRowAffinity EffectiveCaretAffinity() const {
    return caret_wrap_affinity_ == WrapRowAffinity::kPreviousRow &&
                   caret_wrap_affinity_position_.line == cursor_line_ &&
                   caret_wrap_affinity_position_.column == cursor_column_
               ? WrapRowAffinity::kPreviousRow
               : WrapRowAffinity::kNextRow;
  }
  static TextEncoding DetectEncoding(std::string_view content);
  static TextEncoding DetectEncoding(LineSpan lines);
  static bool IsBefore(const TextPosition& lhs, const TextPosition& rhs);

  // Lines that have changed since the folding model last resynced. Consumed (and
  // reset) by `ConsumeFoldEditSpan`.
  LineEditSpan fold_edit_span_;
  std::shared_ptr<DocumentState> document_;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;
  // Sticky column for vertical motion, measured in ON-SCREEN cells of the caret's
  // visual row: under soft wrap it is `hanging indent + offset into the row`, so
  // moving between rows with different hanging indents keeps the caret under the
  // same screen column (VS Code's view-column model) instead of drifting by the
  // indent. Without soft wrap it is the absolute visual column.
  std::size_t preferred_column_ = 0;
  // Wrap-boundary tiebreaker for the primary caret + the position it was set at.
  // See EffectiveCaretAffinity().
  WrapRowAffinity caret_wrap_affinity_ = WrapRowAffinity::kNextRow;
  TextPosition caret_wrap_affinity_position_;
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
  // Shared, not owned: see SetLanguageContractView. Null ⇒ no contract.
  std::shared_ptr<const LanguageContractView> lc_view_;
  // language_id() memo. Keyed on document identity + content revision + path +
  // syntax-registry revision; `language_id_valid_` is the "never resolved yet"
  // flag, since an empty filetype is a legitimate cached answer.
  mutable std::string language_id_;
  mutable const void* language_id_document_ = nullptr;
  mutable std::filesystem::path language_id_path_;
  mutable std::uint64_t language_id_content_revision_ = 0;
  mutable std::size_t language_id_registry_revision_ = 0;
  mutable bool language_id_valid_ = false;
  // Whether the cached answer depended on the buffer's bytes at all. False for an
  // unambiguous extension, which is nearly every file: the memo then survives
  // every content edit. True only for `.h`-style ambiguous extensions and files
  // with no fast candidate set, where it is keyed on `head_content_revision`.
  mutable bool language_id_content_sensitive_ = false;
  std::vector<SecondaryCaret> secondary_carets_;
  ColumnSelectionState column_selection_;
  // Cache for secondary_caret_positions(): mirrors `secondary_carets_.position` and is rebuilt
  // lazily when sizes differ or any element changed. Capacity persists across rebuilds.
  mutable std::vector<TextPosition> secondary_caret_positions_cache_;
  // Scratch buffers for the box/column-selection caret rebuild. That rebuild runs
  // once per keystroke of a held Ctrl+Shift+Alt+Arrow gesture and is O(span) each
  // time; keeping the capacity here makes it allocation-free after the first
  // keystroke instead of allocating twice per keystroke.
  std::vector<SelectionRange> box_ranges_scratch_;
  std::vector<SecondaryCaret> secondary_caret_candidates_scratch_;
  mutable TextLayoutCache layout_cache_;
  // Per-line token cache. Entries carry the generation they were computed in
  // rather than being erased when the document changes: an edit bumps
  // `highlight_cache_generation_`, every entry becomes a miss, and the next paint
  // re-tokenizes each visible line straight back INTO the vector already sitting
  // in the map. Clearing instead freed one hash node plus one token vector per
  // cached line on every keystroke and re-allocated both on the same frame — a
  // screenful of churn per character typed, for a cache whose keys and sizes are
  // almost always identical either side of the edit.
  //
  // A stale entry keeps its token buffer resident until it is evicted, so the
  // retained bytes are the entry cap (kHighlightCacheLimit lines) times the
  // per-line token cap, which is the same ceiling the live cache already had.
  mutable std::unordered_map<std::size_t, HighlightCacheEntry> highlight_cache_;
  // Starts at 1 so a default-constructed entry (generation 0) can never read as
  // current. Bumped by DropHighlightTokenCache; never reset.
  mutable std::uint64_t highlight_cache_generation_ = 1;
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
  std::size_t VisualColumnSpliceDerivedCountForDebug() const {
    return layout_cache_.visual_column_splice_derived_count_for_debug();
  }
#endif
};

}  // namespace microide::editor
