#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineEditSpan.h"
#include "editor/LineSpan.h"

namespace microide::editor {

class TextViewport;

enum class FoldSource : std::uint8_t {
  Bracket = 0,
  Indent = 1,
};

struct FoldRange {
  std::size_t opener_line = 0;
  std::size_t closer_line = 0;
  FoldSource source = FoldSource::Bracket;
};

// Lazy fold-region model owned by the active editor tab. Designed as a
// CPU-frugal lazy compute keyed on a coarse fingerprint (layout revision,
// tab size, language id) so it can be reused across consecutive frames when
// nothing actionable has changed.
class FoldingModel {
 public:
  struct Fingerprint {
    std::uint64_t layout_revision = 0;
    std::size_t tab_size = 4;
    std::string language_id;

    bool operator==(const Fingerprint& other) const {
      return layout_revision == other.layout_revision && tab_size == other.tab_size &&
             language_id == other.language_id;
    }
    bool operator!=(const Fingerprint& other) const { return !(*this == other); }
  };

  struct ComputeOptions {
    // Single-character bracket pairs (e.g. {/}, (/), [/]). Each entry is a
    // (open, close) pair encoded as a 2-character string for ergonomic use
    // in tests; either character can be supplied as the empty string to skip
    // that pair (no-op).
    std::vector<std::pair<char, char>> bracket_pairs;
    // When true, indent-driven block boundaries also yield fold ranges. They
    // are ignored on lines already covered by a balanced bracket range.
    bool use_indent_source = true;
    std::size_t tab_size = 4;
  };

  // Replace the stored ranges with a fresh scan. Returns true on completion.
  bool Compute(LineSpan lines, const ComputeOptions& options);

  // Same as `Compute` but stop scanning once `max_lines` of work is done; the
  // returned ranges are partial and `complete()` will report `false`. This is
  // the budgeted recompute described in the spec; the fold gutter renderer
  // paints whatever is resolved.
  // `edit_span` reports which lines have changed since the last compute (see
  // LineEditSpan). It drives two things: bracket folds whose closer sits before
  // `edit_span.begin()` are reused rather than rescanned, and the per-line
  // indent cache is resynced across the window instead of being remeasured for
  // the whole document. A default-constructed (empty) span means "nothing has
  // changed"; `LineEditSpan::FullRebuild()` forces a whole-file rescan.
  bool ComputeWithBudget(LineSpan lines,
                         const ComputeOptions& options,
                         std::size_t max_lines,
                         const LineEditSpan& edit_span = LineEditSpan::FullRebuild(),
                         std::size_t target_end_exclusive = std::numeric_limits<std::size_t>::max(),
                         const TextViewport* syntax_viewport = nullptr);

  // Resolve folds only for the visible prefix needed by the current viewport
  // plus a bounded look-ahead. When the requested range is already resolved
  // and the model is not dirty, this is a no-op.
  bool EnsureFoldsForVisibleRange(
      LineSpan lines,
      const ComputeOptions& options,
      std::size_t visible_start_line,
      std::size_t visible_end_line,
      std::size_t max_lines,
      const LineEditSpan& edit_span = LineEditSpan::FullRebuild(),
      const TextViewport* syntax_viewport = nullptr);

  // Toggle the collapsed state of the fold range whose opener matches
  // `opener_line`. Returns true if a matching range was found and toggled.
  bool ToggleFold(std::size_t opener_line);
  bool Collapse(std::size_t opener_line);
  bool Expand(std::size_t opener_line);
  void CollapseAll();
  void ExpandAll();

  // Returns true when the line participates in a collapsed fold body (i.e. it
  // is strictly between an opener and a closer of a collapsed range).
  bool IsLineHidden(std::size_t line) const;

  // Returns the fold range whose opener equals `line`, if any.
  std::optional<FoldRange> FoldStartingAt(std::size_t line) const;

  // Returns the innermost fold covering `line` (maximal opener_line among ranges
  // with opener_line <= line <= closer_line). Empty optional when unknown.
  std::optional<FoldRange> InnermostFoldContaining(std::size_t line) const;

  // Append every fold range whose `[opener_line, closer_line]` interval covers
  // `line`, in increasing-opener order (outermost first). Uses the indexed
  // prefix-max-closer cache so callers do not need to scan `ranges()` linearly.
  void AppendFoldsContaining(std::size_t line, std::vector<FoldRange>* out) const;

  // True iff a fold range opens at `line` and is currently collapsed.
  bool IsCollapsedAtOpener(std::size_t line) const;

  void Clear();

  const std::vector<FoldRange>& ranges() const { return ranges_; }
  const std::vector<bool>& collapsed_flags() const { return collapsed_; }
  // O(1) probe for "is any fold collapsed". Replaces the linear scan of
  // `collapsed_flags()` that the wrapped-row layout build used to do every edit.
  bool has_any_collapsed_fold() const { return collapsed_count_ > 0; }
  bool complete() const { return complete_; }
  std::size_t revision() const { return revision_; }

  // Look-ahead the visible-range resolve extends past the last visible line so an
  // opener on-screen whose closer sits just below still gets a marker. Shared with
  // EnsureFoldsForVisibleRange and IsVisibleRangeResolved so the refresh gate and
  // the scan use the same window.
  static constexpr std::size_t kVisibleLookAhead = 32;

  bool IsFresh(const Fingerprint& fingerprint) const {
    return !dirty_ && fingerprint_ == fingerprint;
  }
  // True when the folds covering [0, visible_end + look-ahead] are already resolved,
  // so an EnsureFoldsForVisibleRange call for that range would no-op. A budgeted
  // compute on a huge file resolves only a prefix, so IsFresh (content-only) is not
  // sufficient to skip the refresh — the scan must still extend as the user scrolls.
  bool IsVisibleRangeResolved(std::size_t visible_end_line) const {
    return !dirty_ &&
           (complete_ || resolved_prefix_line_count_ >= visible_end_line + kVisibleLookAhead + 1);
  }
  void MarkDirty() { dirty_ = true; }
  void SetFingerprint(Fingerprint fingerprint) {
    fingerprint_ = std::move(fingerprint);
    dirty_ = false;
  }
  const Fingerprint& fingerprint() const { return fingerprint_; }

 public:
  // One open bracket on the scan stack. Public only so the scanners in the .cpp
  // can name it; it is an implementation detail of the bracket scan.
  struct BracketStackEntry {
    char open = 0;
    char close = 0;
    std::size_t line = 0;
  };

  // One bracket byte found on a line, cached so a recompute re-MATCHES events
  // instead of re-SCANNING bytes. Public for the same reason as the stack entry:
  // the scanners live in the .cpp.
  //
  // Deliberately records the raw byte and not the resolved pair index, so the
  // cache is a pure function of the line's bytes -- the pair set can change
  // (a language switch) without the recorded columns becoming wrong, and the
  // suppression decision (is this bracket inside a string or comment?) stays at
  // match time where it belongs, since the highlight cache it depends on changes
  // as the user scrolls rather than as the document is edited.
  struct CachedBracket {
    std::uint32_t column = 0;
    char byte = 0;
  };

 private:
  struct CollapsedInterval {
    std::size_t lo = 0;  // first hidden line (opener + 1)
    std::size_t hi = 0;  // last hidden line (closer, inclusive)
  };

  // Bring `line_indent_` in sync with a document of `line_count` lines, given the
  // lines that changed. Falls back to a full reset when the span cannot describe
  // the difference (a mismatched common-suffix length means an edit went
  // unreported, so nothing cached can be trusted).
  void SyncLineIndentCache(std::size_t line_count, std::size_t tab_size,
                           const LineEditSpan& edit_span);

  // Splice the bracket cache across `edit_span`, then scan whatever lines below
  // `needed_end` are still uncached (bounded by `max_new_line_scans`; clears
  // `complete` when the budget cuts it short). Returns the number of lines the
  // cache now covers.
  std::size_t SyncLineBracketCache(LineSpan lines,
                                   const std::vector<std::pair<char, char>>& pairs,
                                   const LineEditSpan& edit_span, std::size_t needed_end,
                                   std::size_t max_new_line_scans, bool& complete);

  // Drop cached bracket events for lines at/after `line_count`.
  void TruncateLineBracketCache(std::size_t line_count);

  // Build the revision-keyed lookup tables: sorted collapsed-interval list with
  // prefix `hi` running-max, plus a per-range prefix running-max of `closer_line`
  // for InnermostFoldContaining. Cheap when ranges_/collapsed_ are unchanged.
  void EnsureLookupCache() const;

  // Bracket stack as of the start of `prefix_stack_line_`.
  //
  // The incremental-resume path avoids re-EMITTING fold ranges for the lines
  // before an edit, but it still had to walk every byte of them to know which
  // brackets were open at the edit point -- half a document per keystroke on a
  // mid-file edit. Consecutive keystrokes in one place all resume at the same
  // line, and an edit at or after that line cannot change any byte before it, so
  // the stack computed for it stays valid: reuse it while the resume line is
  // unchanged.
  //
  // The resume line is the MINIMUM line touched since the last refresh consumed
  // it, so an edit anywhere earlier lowers it and the memo stops matching --
  // which is exactly the condition under which the prefix is no longer intact.
  std::vector<BracketStackEntry> prefix_stack_;
  std::size_t prefix_stack_line_ = 0;
  bool prefix_stack_valid_ = false;

  // Per-line bracket bytes for lines [0, bracket_cache_valid_through_).
  //
  // The bracket scan was the single largest cost of typing in a large file (404 ms
  // of the 613 ms mid_file_edit_latency_large_file scenario): a mid-file keystroke
  // re-scanned every byte from the edit to the end of the scan window, ~2 MB on the
  // 50k-line fixture, purely to rediscover bracket positions that had not moved.
  // Caching them turns that byte scan into a walk over a few events per line.
  //
  // Layout is counts + one flat event array rather than per-line offsets, so an
  // edit splices both with plain memmoves and the match walk consumes them in
  // lockstep -- no offset fixup pass. The cache covers a PREFIX of the document so
  // a budgeted first paint still only scans its own window.
  std::vector<CachedBracket> line_brackets_;
  std::vector<std::uint32_t> line_bracket_count_;
  std::size_t bracket_cache_valid_through_ = 0;
  // Bracket pair set the cache was filled against; a change invalidates it.
  std::vector<std::pair<char, char>> bracket_cache_pairs_;
  // Reused splice buffers so a per-keystroke resync does not reallocate.
  std::vector<CachedBracket> bracket_event_scratch_;
  std::vector<std::uint32_t> bracket_count_scratch_;
  // Sorted line indices whose highlight tokens are currently cached, rebuilt per
  // recompute. Walking a cursor through this replaces a hash probe per line.
  std::vector<std::size_t> suppression_lines_scratch_;

  // Per-line leading-indent width, memoized across recomputes.
  //
  // Measuring it for the whole document was the second-largest cost of typing in
  // a large file (ScanIndentRanges::Measure, 141 ms of the 759 ms
  // mid_file_edit_latency_large_file scenario) -- and a line's indent depends on
  // that line's bytes and nothing else, so an edit invalidates only the lines it
  // touched. The reported LineEditSpan splices this array; entries inside the
  // window are reset to `kUnmeasuredIndent` and remeasured on first read, which
  // also keeps a budgeted first paint from measuring past its scan window.
  std::vector<std::uint32_t> line_indent_;
  // Tab size the cached widths were measured against; a change invalidates all.
  std::size_t line_indent_tab_size_ = 0;

  std::vector<FoldRange> ranges_;
  // Reused across recomputes: opener_line -> index of the winning range, used by
  // the bucket dedupe that replaced the O(n log n) sort. Held on the model so a
  // per-keystroke recompute does not reallocate a document-sized scratch.
  std::vector<std::uint32_t> merge_by_opener_scratch_;
  // Destination buffer for that compaction. Swapped with `ranges_` rather than
  // assigned, so the two buffers alternate and neither reallocates in steady
  // state.
  std::vector<FoldRange> merge_compact_scratch_;
  std::vector<bool> collapsed_;  // parallel to ranges_
  // Maintained alongside `collapsed_` so `has_any_collapsed_fold()` is O(1).
  std::size_t collapsed_count_ = 0;
  Fingerprint fingerprint_;
  bool complete_ = true;
  bool dirty_ = true;
  std::size_t revision_ = 0;
  std::size_t resolved_prefix_line_count_ = 0;
  // Line count of the buffer at the last compute. The difference against the
  // current line count gives the net edit delta used to shift previous collapsed
  // fold openers so a collapsed fold survives a line-count-changing edit above it
  // (VS Code shift-preserves; matching by absolute opener/closer alone dropped it).
  std::size_t computed_line_count_ = 0;

  mutable std::size_t cached_revision_ = std::numeric_limits<std::size_t>::max();
  mutable std::vector<CollapsedInterval> cached_collapsed_intervals_;
  mutable std::vector<std::size_t> cached_collapsed_hi_prefix_max_;
  mutable std::vector<std::size_t> cached_range_closer_prefix_max_;
};

}  // namespace microide::editor
