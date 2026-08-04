#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
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

// Incremental fold-region model owned by the active editor tab.
//
// # Why this shape
//
// Folds are a pure function of two per-line facts: the bracket bytes on a line,
// and the line's leading indent width. Both are cached per line and spliced
// across an edit (see `LineEditSpan`), so a keystroke re-measures only the lines
// it touched. The expensive part used to be everything *after* that: deriving
// the fold ranges from those per-line facts re-walked the whole document on
// every keystroke -- match every bracket, scan every indent block, then sort and
// dedupe a document-sized range list -- which was ~26% of a keystroke on a
// 50k-line file and O(document) by construction, not by constant factor.
//
// The derivation is now incremental, in two steps.
//
// **Blocks.** The document is partitioned into variable-size blocks of ~256
// lines. Each block stores its *reduced word*: the brackets it leaves unmatched
// (closers reaching before it, openers still open at its end) and the same for
// indent levels. Reducing a line range that way is a monoid -- combining two
// adjacent words cancels the right side's closers against the left side's
// openers -- and it is the same stack machine the line-by-line scan runs, so
// combining block words yields exactly the state a full scan would reach. An
// edit invalidates only the block it lands in; every other block's word stays
// valid, because a block's word is a pure function of its own lines. Words hold
// line numbers *relative to their block*, so lines shifting above a block never
// touch it.
//
// **Windowed resolution.** The model no longer materialises every fold range in
// the document. It resolves the ranges relevant to a line window: the folds
// opening inside it, plus the folds containing its first line (the sticky-scroll
// ancestors). That is the whole per-frame need, and it makes the per-frame cost
// O(window) rather than O(document). Reaching the window's incoming state is a
// walk over block words, memoised at each block boundary, and resolving an
// opener whose closer sits far below consumes whole block words rather than
// lines -- a top-level `{` closing 50k lines later costs ~200 word applications,
// not 50k line visits.
//
// # Collapsed state
//
// The set of collapsed folds is its own document-wide list, independent of what
// is currently resolved. It used to be a parallel bit-vector over the resolved
// range list, remapped by re-matching openers after every recompute, which meant
// a collapsed fold outside the resolved prefix of a large file silently
// re-expanded. Now an edit shifts the collapsed ranges by the edit delta, and a
// resolve revalidates only the ones whose opener lands inside its window.
class FoldingModel {
 public:
  // `last_line` value meaning "through the end of the document".
  static constexpr std::size_t kAllLines = std::numeric_limits<std::size_t>::max();
  // "no such line", in the block-relative 32-bit and absolute 64-bit forms.
  static constexpr std::uint32_t kNoLine = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::size_t kNoLineIndex = std::numeric_limits<std::size_t>::max();

  // Extra lines resolved on each side of a requested window, so scrolling a few
  // rows reuses the previous resolve instead of redoing it every frame.
  static constexpr std::size_t kWindowPadLines = 96;

  // Declared cap on how many enclosing constructs a walk tracks at once.
  //
  // Everything a fold consumer asks for is INNERMOST-first: the gutter wants the
  // folds opening in the viewport, sticky scroll wants at most eight enclosing
  // openers, `InnermostFoldContaining` wants one. So the outermost entries of a
  // very deep nest are resolvable in principle and useful to nobody -- while
  // carrying them costs a copy per refresh, a memoised state per block, and a
  // forward walk that cannot stop until the outermost one closes, which for a
  // construct spanning the file means end-of-document every frame.
  //
  // Real source sits under ~50 levels. The 50k-line perf fixture is 8300 braces
  // deep on purpose, which is what made the cost of not capping visible.
  static constexpr std::size_t kMaxOpenDepth = 256;

  // Declared cap on how far past its window a resolve chases a closer.
  //
  // Knowing where a construct opened above the viewport closes means reading
  // every byte in between -- there is no summary that can be had for less. That
  // is cheap for the ordinary case (a function's `}` is tens of lines down) and
  // it is what lets a top-level construct get a fold marker on the first frame
  // instead of only once the viewport scrolled near its closer. But a construct
  // spanning an entire large file would make the first frame read the whole
  // file, so stop at a bound that covers whole real files and leave anything
  // longer to resolve when the viewport moves nearer to it.
  //
  // Hitting this bound is NOT a partial resolve: it is the answer for this
  // window, so the refresh gate must not keep asking.
  static constexpr std::size_t kMaxForwardResolveLines = 8192;

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
    // Single-character bracket pairs (e.g. {/}, (/), [/]).
    std::vector<std::pair<char, char>> bracket_pairs;
    // When true, indent-driven block boundaries also yield fold ranges. They are
    // dropped when a bracket range already covers the same opener line.
    bool use_indent_source = true;
    std::size_t tab_size = 4;
  };

  // Resolve the fold ranges covering `[first_line, last_line]` (clamped to the
  // document; `kAllLines` means end-of-document), reusing the incremental caches.
  //
  // `edit_span` reports which lines changed since the previous call: an empty
  // span means "nothing changed", `LineEditSpan::FullRebuild()` drops every
  // cache. `max_lines` bounds how many *uncached* lines this call may scan or
  // measure (0 = unbounded); when the bound cuts the work short the resolved
  // ranges are partial and `complete()` reports false -- and the next call gets
  // twice the bound, so catching up after a jump takes O(log n) frames rather
  // than O(n / budget). `syntax_viewport`, when non-null, suppresses brackets
  // inside strings and comments for the lines whose syntax tokens are cached.
  //
  // Returns `complete()`.
  bool Refresh(LineSpan lines,
               const ComputeOptions& options,
               std::size_t first_line,
               std::size_t last_line,
               std::size_t max_lines = 0,
               const LineEditSpan& edit_span = LineEditSpan::FullRebuild(),
               const TextViewport* syntax_viewport = nullptr);

  // Resolve every fold range in the document. O(document); for tests, whole-file
  // operations, and documents small enough that a window is not worth having.
  bool Compute(LineSpan lines, const ComputeOptions& options) {
    return Refresh(lines, options, 0, kAllLines, /*max_lines=*/0, LineEditSpan::FullRebuild(),
                   nullptr);
  }

  // Resolve every fold in the document reusing the options of the previous
  // refresh, and without invalidating any cache. This is the `Fold All` /
  // `Unfold All` command path: O(document) and unbudgeted, but driven only by an
  // explicit user command -- and it keeps the fold options at the one place that
  // already derives them from the language contract, rather than threading a
  // second copy through the action layer.
  bool ResolveAllFolds(LineSpan lines, const TextViewport* syntax_viewport = nullptr);

  // ---- queries over the resolved window ---------------------------------

  // The fold whose opener is `line`, if one is resolved.
  std::optional<FoldRange> FoldStartingAt(std::size_t line) const;
  // The innermost resolved fold whose `[opener, closer]` interval covers `line`.
  std::optional<FoldRange> InnermostFoldContaining(std::size_t line) const;
  // Every resolved fold covering `line`, outermost first.
  void AppendFoldsContaining(std::size_t line, std::vector<FoldRange>* out) const;

  // Ranges resolved by the last `Refresh`, sorted by opener line, one per opener.
  const std::vector<FoldRange>& resolved_ranges() const { return ranges_; }
  // The line window `resolved_ranges()` covers.
  std::size_t resolved_first_line() const { return resolved_first_line_; }
  std::size_t resolved_last_line() const { return resolved_last_line_; }
  // True when the last resolve covers `[first_line, last_line]` and no edit has
  // landed since, so the refresh gate can skip a redundant resolve.
  bool IsWindowResolved(std::size_t first_line, std::size_t last_line) const {
    return resolved_ && !dirty_ && first_line >= resolved_first_line_ &&
           (last_line <= resolved_last_line_ || resolved_last_line_ + 1 >= document_line_count_);
  }

  // ---- collapsed state (document-wide) ----------------------------------

  bool ToggleFold(std::size_t opener_line);
  bool Collapse(std::size_t opener_line);
  bool Expand(std::size_t opener_line);
  // Collapse a range the caller already holds. Unlike `Collapse` this does not
  // require the fold to be inside the resolved window: it is what restores a
  // fold the caller temporarily expanded (buffer-search reveal) once the
  // viewport has moved away from it.
  bool CollapseRange(FoldRange range);
  // Collapse every fold in the *resolved* window. `Fold All` resolves the whole
  // document first (see `workspace::ResolveAllFolds`) so this means every fold.
  bool CollapseAllResolved();
  void ExpandAll();

  // True when `line` sits strictly inside a collapsed fold body.
  bool IsLineHidden(std::size_t line) const;
  bool IsCollapsedAtOpener(std::size_t line) const;
  bool has_any_collapsed_fold() const { return !collapsed_.empty(); }
  std::span<const FoldRange> collapsed_ranges() const { return collapsed_; }

  void Clear();

  // True when the model carries anything the viewport must lay out or paint.
  bool HasFolds() const { return !ranges_.empty() || !collapsed_.empty(); }
  bool complete() const { return complete_; }

  // Bumps whenever anything a fold consumer reads changes: the resolved window
  // or the collapsed set. Keys the render view-model and sticky-scroll caches.
  std::size_t revision() const { return revision_; }
  // Bumps only when the *collapsed* set changes, i.e. when the set of hidden
  // lines changes. Keys the wrapped-row layout cache, which must not be rebuilt
  // just because a scroll resolved a different window.
  std::size_t layout_revision() const { return layout_revision_; }

  bool IsFresh(const Fingerprint& fingerprint) const {
    return !dirty_ && fingerprint_ == fingerprint;
  }
  void MarkDirty() { dirty_ = true; }
  void SetFingerprint(Fingerprint fingerprint) {
    fingerprint_ = std::move(fingerprint);
    dirty_ = false;
  }
  const Fingerprint& fingerprint() const { return fingerprint_; }

  // ---- implementation types --------------------------------------------
  //
  // Public only so the scanners and walkers in the .cpp can name them; they are
  // implementation detail of the block partition.

  // One bracket byte found on a line, cached so a recompute re-MATCHES events
  // instead of re-SCANNING bytes.
  //
  // Deliberately records the raw byte and not the resolved pair index, so the
  // cache is a pure function of the line's bytes: the pair set can change (a
  // language switch) without the recorded columns becoming wrong, and the
  // suppression decision (is this bracket inside a string or comment?) stays at
  // match time where it belongs, since the highlight cache it depends on changes
  // as the user scrolls rather than as the document is edited.
  struct CachedBracket {
    std::uint32_t column = 0;
    char byte = 0;
  };

  // A bracket left open by a block. `line` is relative to the block start.
  struct WordOpener {
    char close = 0;
    std::uint32_t line = 0;
  };

  // A closer a block could not match against anything inside itself. Whether it
  // pops or is discarded is decided when the word meets a real incoming stack,
  // exactly as the line-by-line scanner decides it.
  struct WordCloser {
    char close = 0;
    std::uint32_t line = 0;
  };

  // An indent level left open by a block. `line` is relative to the block start.
  struct WordIndent {
    std::uint32_t level = 0;
    std::uint32_t line = 0;
  };

  // A dedent reaching before the block that recorded it: every pending indent
  // level at or above `threshold` closes, at `closer_line`. `closer_line ==
  // kNoLine` means "the last non-blank line before this block", which the walk
  // substitutes from its incoming state.
  struct WordDedent {
    std::uint32_t threshold = 0;
    std::uint32_t closer_line = 0;
  };

  // Live walk state entries carry ABSOLUTE line numbers, so they are 64-bit
  // where the block-relative word entries are 32-bit.
  struct StackBracket {
    char close = 0;
    std::size_t line = 0;
  };
  struct StackIndent {
    std::uint32_t level = 0;
    std::size_t line = 0;
  };

  // How many uncached lines a call may still scan or measure. `kUnlimited`
  // disables the bound; a bounded budget makes the caller mark the result
  // partial rather than stall a frame on a huge file.
  struct WorkBudget {
    static constexpr std::size_t kUnlimited = std::numeric_limits<std::size_t>::max();
    std::size_t remaining = kUnlimited;

    bool available() const { return remaining != 0; }
    void Spend(std::size_t lines = 1) {
      if (remaining == kUnlimited) {
        return;
      }
      remaining = lines >= remaining ? 0 : remaining - lines;
    }
  };

 private:
  // The reduced word of one run of lines, plus what a walk needs to skip over it:
  // how many lines and bracket events it holds, and where its last non-blank line
  // sits (the closer an indent fold ending in a later block gets).
  struct Block {
    std::uint32_t line_count = 0;
    std::uint32_t event_count = 0;
    bool valid = false;
    std::uint32_t last_nonblank = kNoLine;  // relative to the block start
    std::vector<WordCloser> bracket_closers;
    std::vector<WordOpener> bracket_openers;
    std::vector<WordDedent> indent_dedents;
    std::vector<WordIndent> indent_openers;
  };

  // The walk state at a block boundary, sliced out of shared pools so a document
  // with thousands of blocks does not hold thousands of small vectors.
  struct PrefixState {
    std::uint32_t bracket_offset = 0;
    std::uint32_t bracket_count = 0;
    std::uint32_t indent_offset = 0;
    std::uint32_t indent_count = 0;
    std::size_t event_index = 0;
    std::size_t last_nonblank = kNoLineIndex;
  };

  // Live scan state: the two stacks plus the last non-blank line seen.
  struct WalkState {
    std::vector<StackBracket> brackets;
    std::vector<StackIndent> indents;
    std::size_t last_nonblank = kNoLineIndex;

    // How many entries opened at or before `pending_limit` are still on each
    // stack. The forward walk past the window is done exactly when both reach
    // zero: every fold the window can name has found its closer.
    //
    // This has to be a COUNT, not "has the stack been empty". Every non-blank
    // line pushes an indent entry, so the indent stack is never empty after one
    // -- a size-based test can never fire, and the walk runs to end-of-document
    // on every frame. Pops are LIFO and forward-walk pushes are all past the
    // limit, so decrementing on a pop below the limit is exact.
    std::size_t pending_limit = 0;
    std::size_t pending_brackets = 0;
    std::size_t pending_indents = 0;

    void ArmPending(std::size_t limit) {
      pending_limit = limit;
      pending_brackets = brackets.size();
      pending_indents = indents.size();
    }

    // Push, dropping the outermost half when the stack passes twice the cap so
    // the trim is amortised O(1) rather than a shift per push. Dropped entries
    // stop being pending: they can no longer be resolved, and the forward walk
    // must not wait on them.
    void PushBracket(StackBracket entry) {
      brackets.push_back(entry);
      if (brackets.size() > 2 * kMaxOpenDepth) {
        brackets.erase(brackets.begin(),
                       brackets.begin() + static_cast<std::ptrdiff_t>(kMaxOpenDepth));
        pending_brackets = pending_brackets > kMaxOpenDepth ? pending_brackets - kMaxOpenDepth : 0;
      }
    }
    void PushIndent(StackIndent entry) {
      indents.push_back(entry);
      if (indents.size() > 2 * kMaxOpenDepth) {
        indents.erase(indents.begin(),
                      indents.begin() + static_cast<std::ptrdiff_t>(kMaxOpenDepth));
        pending_indents = pending_indents > kMaxOpenDepth ? pending_indents - kMaxOpenDepth : 0;
      }
    }
    bool pending_resolved() const { return pending_brackets == 0 && pending_indents == 0; }
    void NoteBracketPop(std::size_t line) {
      if (pending_brackets != 0 && line <= pending_limit) {
        --pending_brackets;
      }
    }
    void NoteIndentPop(std::size_t line) {
      if (pending_indents != 0 && line <= pending_limit) {
        --pending_indents;
      }
    }

    void Clear() {
      brackets.clear();
      indents.clear();
      last_nonblank = kNoLineIndex;
      pending_limit = 0;
      pending_brackets = 0;
      pending_indents = 0;
    }
  };

  class BracketTable;
  class SuppressionCursor;

  // ---- per-line cache sync ----------------------------------------------
  void SyncLineIndentCache(std::size_t line_count, std::size_t tab_size,
                           const LineEditSpan& edit_span);
  void SyncLineBracketCache(LineSpan lines, const BracketTable& table,
                            const std::vector<std::pair<char, char>>& pairs,
                            const LineEditSpan& edit_span, bool pairs_changed);
  bool ExtendBracketCache(LineSpan lines, const BracketTable& table, std::size_t needed_end,
                          WorkBudget& budget);
  std::uint32_t IndentAt(LineSpan lines, std::size_t line, std::size_t tab_size,
                         WorkBudget& budget);
  std::size_t EventIndexForLine(std::size_t line) const;

  // ---- block partition ---------------------------------------------------
  void SyncBlocks(std::size_t line_count, const LineEditSpan& edit_span, bool reset_all);
  void RepartitionAll(std::size_t line_count);
  void RecomputeBlockEventCount(std::size_t block_index);
  void EnsureBlockStarts() const;
  std::size_t BlockIndexForLine(std::size_t line) const;
  bool EnsureBlockSummary(LineSpan lines, const ComputeOptions& options, const BracketTable& table,
                          std::size_t block_index, std::size_t event_index, WorkBudget& budget);
  // Memoise the walk state at each block boundary up to `boundary`. Returns the
  // highest boundary whose state is known (== `boundary` on success).
  std::size_t EnsurePrefixState(LineSpan lines, const ComputeOptions& options,
                                const BracketTable& table, std::size_t boundary,
                                WorkBudget& budget);
  void LoadPrefixState(std::size_t boundary, WalkState& out) const;
  void StorePrefixState(std::size_t boundary, const WalkState& state, std::size_t event_index);
  void InvalidatePrefixFrom(std::size_t boundary);

  // ---- walking -----------------------------------------------------------
  void WalkLines(LineSpan lines, const ComputeOptions& options, const BracketTable& table,
                 SuppressionCursor& suppression, std::size_t begin, std::size_t end,
                 std::size_t& event_index, WalkState& state, std::vector<FoldRange>* out,
                 std::size_t emit_opener_limit, WorkBudget& budget);
  void ApplyBlockWord(const Block& block, std::size_t block_start, WalkState& state,
                      std::vector<FoldRange>* out, std::size_t emit_opener_limit) const;
  void FlushIndentsAtEof(WalkState& state, std::vector<FoldRange>* out,
                         std::size_t emit_opener_limit) const;

  void FinishRanges();
  void ShiftCollapsedRanges(const LineEditSpan& edit_span, std::size_t line_count);
  void RevalidateCollapsedInWindow();
  void EnsureCollapsedIndex() const;
  void BumpCollapseRevision();

  // ---- per-line caches ---------------------------------------------------
  std::vector<CachedBracket> line_brackets_;
  std::vector<std::uint32_t> line_bracket_count_;
  std::size_t bracket_cache_valid_through_ = 0;
  std::vector<std::pair<char, char>> bracket_cache_pairs_;
  std::vector<CachedBracket> bracket_event_scratch_;
  std::vector<std::uint32_t> bracket_count_scratch_;
  // Per-line leading-indent width, measured lazily and memoised across
  // recomputes; a line's indent depends on that line's bytes and nothing else,
  // so an edit invalidates only the lines it touched.
  std::vector<std::uint32_t> line_indent_;
  std::size_t line_indent_tab_size_ = 0;

  // ---- block partition ---------------------------------------------------
  std::vector<Block> blocks_;
  mutable std::vector<std::size_t> block_start_line_;
  mutable bool block_starts_dirty_ = true;

  std::vector<PrefixState> prefix_states_;
  std::vector<StackBracket> prefix_bracket_pool_;
  std::vector<StackIndent> prefix_indent_pool_;

  // ---- resolved window ---------------------------------------------------
  std::vector<FoldRange> ranges_;
  std::vector<FoldRange> range_scratch_;
  // Prefix running max of `closer_line` over `ranges_`, so the ancestor walk can
  // stop instead of scanning back to the first range.
  std::vector<std::size_t> range_closer_prefix_max_;
  std::size_t resolved_first_line_ = 0;
  std::size_t resolved_last_line_ = 0;
  bool resolved_ = false;

  // ---- collapsed set -----------------------------------------------------
  std::vector<FoldRange> collapsed_;  // sorted by opener_line, unique openers
  mutable std::vector<std::size_t> collapsed_hi_prefix_max_;
  mutable bool collapsed_index_dirty_ = true;

  // ---- scratch -----------------------------------------------------------
  WalkState walk_;
  // A block's word is built into these and then `assign`ed into the block, so a
  // block costs at most one allocation per non-empty word list instead of a
  // push_back growth series. Building every block of a 50k-line file is a
  // one-off, but it is a one-off per opened document and it showed up as a 21%
  // allocation regression on the fold perf gate.
  std::vector<StackBracket> build_bracket_stack_;
  std::vector<StackIndent> build_indent_stack_;
  std::vector<WordCloser> build_bracket_closers_;
  std::vector<WordOpener> build_bracket_openers_;
  std::vector<WordDedent> build_indent_dedents_;
  std::vector<WordIndent> build_indent_openers_;
  std::vector<std::size_t> suppression_lines_scratch_;

  Fingerprint fingerprint_;
  std::size_t document_line_count_ = 0;
  // Multiplier applied to the caller's per-refresh budget; doubles while the
  // model is still catching up so a jump into a huge file converges in O(log n)
  // frames instead of O(n / budget).
  std::size_t budget_multiplier_ = 1;
  // Whether the block words currently hold indent entries. Turning the indent
  // source off must drop them, or a stale word would keep applying dedents.
  bool indent_source_enabled_ = true;
  bool complete_ = true;
  bool dirty_ = true;
  std::size_t revision_ = 0;
  std::size_t layout_revision_ = 0;
};

}  // namespace microide::editor
