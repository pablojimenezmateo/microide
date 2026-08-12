#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"

namespace microide::editor {

// Typing-coalesce classification for a single recorded edit. A run of edits
// sharing the same kind, advancing contiguously from the caret, is merged into
// one undo entry so that undo removes a word/run at a time rather than a
// character at a time. `None` always starts (and ends) a fresh entry.
enum class CoalesceKind { None, Insert, DeleteBackward, DeleteForward };

struct CoalesceHint {
  CoalesceKind kind = CoalesceKind::None;
  // Whitespace class of the single character this edit added or removed. Used
  // to split a run before a new word: a non-space char following a space (e.g.
  // the 'b' in "foo bar") begins a fresh undo entry.
  bool changed_is_space = false;
};

// Owns the undo / redo storage and the grouped-edit aggregation state that
// previously lived on TextViewport (undo_stack, redo_stack, undo_group_stack_,
// plus the static TryMerge / ReconstructFallback / ApplyEntryToBuffer helpers).
//
// The owning TextViewport stays in charge of:
//   - capturing a ViewState snapshot from its current cursor / scroll /
//     selection / secondary-caret state,
//   - building the line slices that form a HistoryEntry,
//   - applying a popped HistoryEntry back onto its line buffer and view state
//     (because that touches private viewport caches),
// and delegates pure storage / grouping / merge math to this type. Mirrors the
// extraction pattern of TextViewport{FileIO,HighlightCache,Multicaret,ViewState,
// LanguageBehavior} that landed across 2026-05-18, but actually moves
// ownership off the viewport rather than just rearranging files.
class TextViewportUndoHistory {
 public:
  struct SecondaryCaret {
    TextPosition position;
    std::size_t preferred_column = 0;
    std::optional<TextPosition> selection_anchor;
    // Wrap-boundary tiebreaker, same meaning as the primary caret's (see
    // WrapRowAffinity). Only vertical motion sets it; every other path that
    // builds or moves a secondary caret leaves it at the kNextRow default, which
    // is the reset.
    WrapRowAffinity wrap_affinity = WrapRowAffinity::kNextRow;
  };

  struct ViewState {
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    std::size_t preferred_column = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::optional<TextPosition> selection_anchor;
    std::vector<SecondaryCaret> secondary_carets;
    bool placeholder = false;
    bool dirty = false;
  };

  // One undo step. It has two shapes, and which one it is decides whether a
  // keystroke costs a few bytes or a few megabytes.
  //
  // COLUMN-SCOPED ("inline", `is_inline == true`). The entry describes a single
  // in-line splice: on line `start_line`, the bytes
  // [`start_column`, `start_column` + `removed_text.size()`) become
  // `inserted_text`. Neither string contains a '\n', and the line vectors stay
  // empty. This is what an ordinary keystroke produces.
  //
  // LINE-VECTOR (`is_inline == false`). `before_lines` at `start_line` become
  // `after_lines`. Every structural edit lives here: line splits and joins,
  // multi-line replaces, grouped/multi-caret aggregates, whole-document changes.
  //
  // The line form cannot express "one character changed at a column" without
  // storing the entire affected line twice, so a one-character edit used to
  // allocate five copies of its line: the pre-edit slice, the composed post-edit
  // line, the tree's replacement buffer, its add-buffer append, and the typing
  // run's coalesce. Three of those are gone with the column-scoped form, and the
  // other two with the piece tree's column-scoped splice (TD-2026-08-05-131).
  //
  // A LINE-VECTOR entry may also be MULTI-RANGE: see `Entry::extra_parts`.

  // One additional disjoint line-range replacement inside a LINE-VECTOR entry.
  struct DisjointPart {
    // Where this part's `before_lines` sit in the PRE-edit document, and where its
    // `after_lines` sit in the POST-edit document. The two differ by the net line
    // delta of every part BELOW this one, which is why both are stored: a forward
    // apply walks the parts highest-first using `before_start`, an undo walks them
    // highest-first using `after_start`, and in each direction the parts still to
    // come sit below and are therefore unshifted by the ones already applied.
    std::size_t before_start = 0;
    std::size_t after_start = 0;
    LineBlob before_lines;
    LineBlob after_lines;
  };

  struct Entry {
    std::size_t start_line = 0;
    bool is_inline = false;
    std::size_t start_column = 0;
    std::string removed_text;
    std::string inserted_text;
    // Blob-backed, not one owned string per line: a line-shaped edit costs 2
    // allocations per direction instead of 2n, and undo/redo frees two blocks
    // instead of n (TD-2026-08-11-182).
    LineBlob before_lines;
    LineBlob after_lines;
    // Disjoint ranges this entry ALSO replaces, strictly above the primary range
    // and sorted ascending. Empty for every entry except a non-contiguous
    // multi-caret or grouped edit (TD-2026-08-06-157).
    //
    // The line form can only say "these lines became those lines at this index",
    // so a multi-caret edit used to be recorded as one replacement spanning first
    // caret line to last: eight carets 8,400 lines apart captured 8,401 lines for
    // the before image and 8,401 for the after, and RETAINED both against the
    // history byte budget, on every keystroke. Ctrl+Shift+L on a common token puts
    // carets across the whole document, which made each subsequent keystroke copy
    // it twice. The interior lines are identical in both images by construction —
    // the group frame already carried the edit as a disjoint set and then stitched
    // it back into one span — so this is that set, kept.
    //
    // The primary range is always the LOWEST, which is what makes `start_line`
    // valid in both coordinate frames (nothing below it moves).
    std::vector<DisjointPart> extra_parts;
    ViewState before_state;
    ViewState after_state;
    // Cached content byte size, stamped when the entry enters the undo stack so
    // the byte-budget trim stays O(entries). Meaningful only while stacked.
    std::size_t byte_size = 0;

    // Lines the entry replaces / installs. An inline entry is exactly one line
    // wide on both sides by construction, so every line-arithmetic consumer
    // (cache invalidation, wrapped-row splice, group bookkeeping) reads these
    // instead of the vectors' sizes.
    std::size_t before_line_count() const { return is_inline ? 1 : before_lines.size(); }
    std::size_t after_line_count() const { return is_inline ? 1 : after_lines.size(); }

    // A multi-range entry. Every consumer that treats an entry as ONE contiguous
    // splice — the coalescing predicates, the applied-edit span published to
    // marker consumers — has to ask this and take its conservative branch.
    bool is_multi_range() const { return !extra_parts.empty(); }

    // The parts in apply order (highest first), as
    // (start, removed_count, inserted_lines). The buffer splice and the derived
    // caches must walk them in this order so each part's index is still valid
    // when it is reached. `forward` selects the direction.
    template <typename Fn>
    void ForEachPartInApplyOrder(bool forward, Fn&& fn) const {
      for (std::size_t i = extra_parts.size(); i-- > 0;) {
        const DisjointPart& part = extra_parts[i];
        fn(forward ? part.before_start : part.after_start,
           forward ? part.before_lines : part.after_lines,
           forward ? part.after_lines : part.before_lines);
      }
      fn(start_line, forward ? before_lines : after_lines, forward ? after_lines : before_lines);
    }
  };

  // Grouping ------------------------------------------------------------
  bool IsGroupActive() const { return !group_stack_.empty(); }
  void BeginGroup(ViewState before_state);
  // Records a candidate child edit. While a group is active this merges into
  // the frame's disjoint-range aggregate; with no group active it either
  // coalesces into the top undo entry (per `hint`) or pushes a fresh entry,
  // clearing the redo stack either way (same base semantics as the prior
  // PushHistoryEntry).
  void RecordEntry(Entry entry, CoalesceHint hint = CoalesceHint{});
  // Bypass-grouping push (matches PushHistoryEntryDirect): clears redo,
  // pushes onto undo, enforces the history cap. Used by paths that have
  // already built a known-good aggregate (e.g. ResetState / Save).
  void RecordEntryDirect(Entry entry);
  // Closes the innermost group. Returns the aggregate Entry the caller
  // should push onto the undo stack via RecordEntryDirect (or nullopt if
  // the group ended up as a no-op).
  std::optional<Entry> FinishActiveGroup(ViewState after_state);

  // Undo / redo stack access -------------------------------------------
  const Entry* TopUndoEntry() const { return undo_stack_.empty() ? nullptr : &undo_stack_.back(); }
  bool CanUndo() const { return !undo_stack_.empty(); }
  bool CanRedo() const { return !redo_stack_.empty(); }
  Entry PopUndo();
  Entry PopRedo();
  void PushUndo(Entry entry);
  void PushRedo(Entry entry);
  void ClearRedo() { redo_stack_.clear(); }
  void Clear();
  // Re-baseline the dirty flags so the current undo/redo position is the single
  // clean point and every other reachable position reads dirty. Called after a
  // successful save so that undoing PAST the save (or redoing forward) correctly
  // reports the buffer as differing from disk.
  void MarkSaved();

  // Heap bytes both stacks are holding, including the group scratch. Part of
  // the per-tab retention accounting TD-2026-08-06-142 asked for, and the one
  // component here that already HAS a declared ceiling: kMaxHistoryBytes, 256
  // MiB, per tab. That cap is what makes the aggregate question sharp rather
  // than academic -- nothing bounds the sum across open tabs, so the declared
  // worst case is kMaxOpenTabsPerGroup times this.
  std::size_t ApproximateResidentBytes() const;

  // End any open typing/deletion coalesce run so the next edit starts a fresh
  // undo entry. Called when the user explicitly moves the caret (arrow keys,
  // Home/End): without it, typing `a`, moving away and back to the same column,
  // then typing `b` would merge `a`+`b` into one undo step.
  void NotifyCursorMoved() { EndCoalesceRun(); }

  // Pure helpers -------------------------------------------------------
  // Applies an entry by mutating the document's TextBuffer in place through its
  // splice primitive. Used by the live undo/redo apply path.
  static void ApplyEntryToBuffer(TextBuffer& lines, const Entry& entry, bool forward);
  static std::optional<AppliedEdit> BuildAppliedEdit(const Entry& entry, bool forward);
  // Whole-line-trimmed line span of `entry` (common leading/trailing identical
  // lines dropped), offset into document coordinates. Equivalent to running
  // ComputeChangedLineSpan over the whole before/after document — the entry's
  // slice already isolates the changed region, so trimming within it and adding
  // start_line yields the identical span at O(entry) cost. nullopt for a no-op.
  static std::optional<AppliedEditLineSpan> BuildAppliedEditLineSpan(const Entry& entry,
                                                                     bool forward);
  // Takes both line vectors BY VALUE and moves the trimmed sub-range out of each.
  // Every caller slices them out of the piece tree and has no further use for
  // them, and on a multi-caret edit whose carets sit far apart each vector is the
  // whole span between the outermost carets — copying them here doubled the cost
  // of the edit for nothing.
  static Entry BuildEntryForDocumentChange(LineBlob before_lines,
                                            const ViewState& before_state,
                                            LineBlob after_lines,
                                            const ViewState& after_state);

 private:
  struct UndoGroupFrame {
    ViewState state;
    // Sorted-ascending, pairwise-non-adjacent set of contiguous aggregate edits,
    // all expressed in the current buffer's after-coordinates. A wholly contiguous
    // group keeps a single element (the old fast path); non-contiguous grouped
    // edits (multi-caret/snippet across gaps) accumulate as separate ranges instead
    // of collapsing to a whole-buffer snapshot. FinishActiveGroup hands them
    // straight to the Entry as its primary range plus `extra_parts`; it used to
    // stitch them into one contiguous range by re-reading every untouched gap line
    // into both images (TD-2026-08-06-157).
    std::vector<Entry> disjoint_entries;
  };

  // Split in two so the merge can consume the aggregate instead of copying it.
  // A single TryMergeGroupEntry had to deep-copy both line vectors before it knew
  // whether the merge was even possible, and typing coalesces on every keystroke.
  // Call the predicate first; the merge's precondition is that it returned true.
  static bool CanMergeGroupEntry(const Entry& aggregate, const Entry& next);
  static Entry MergeGroupEntry(Entry aggregate, const Entry& next);
  // Column-scoped counterparts, used only by the typing/deletion coalesce run
  // (grouped edits widen their children to the line form before recording, so
  // the disjoint-range bookkeeping never sees an inline entry).
  //
  // `aggregate` describes line L as [c0, c0 + |R0|) -> I0 in PRE-run coordinates;
  // once applied, its inserted text occupies [c0, c0 + |I0|), and `next` is stated
  // in those post-aggregate coordinates. Three shapes compose:
  //   * `next` abuts on the right (an insert run, and a delete-forward run),
  //   * `next` abuts on the left (a backspace run),
  //   * `next` lies wholly inside the aggregate's insertion (a re-edit of text
  //     the run itself typed).
  static bool CanMergeInlineEntry(const Entry& aggregate, const Entry& next);
  static void MergeInlineEntryInto(Entry& aggregate, const Entry& next);
  // Fold a freshly recorded child edit into a frame's disjoint-range set: splice
  // into a containing/adjacent range or insert a new one, reindexing strictly-lower
  // ranges by the child's net line delta. O(#ranges), never a whole-buffer copy.
  static void MergeChildIntoDisjoint(std::vector<Entry>& entries, Entry next);
  // Insert `next` keeping `entries` sorted ascending by start_line.
  static void InsertSortedDisjoint(std::vector<Entry>& entries, Entry next);
  // Merge any now-adjacent (gap-free) neighbours so the disjoint invariant holds.
  static void CoalesceAdjacentDisjoint(std::vector<Entry>& entries);
  // Attempts to fold `next` into the current undo-stack top as a continuation
  // of an open typing/deletion run. Returns true (and mutates the top entry)
  // on success; false means the caller should push `next` as a fresh entry.
  bool TryCoalesceWithTop(const Entry& next, CoalesceHint hint);
  void EndCoalesceRun() { active_run_kind_ = CoalesceKind::None; }
  // Push onto the undo stack, stamping byte_size, then enforce both the entry-count
  // and total-byte budgets (evicting oldest). All undo pushes route through here.
  void AppendUndoEntry(Entry entry);
  void EnforceHistoryBudget();

  static constexpr std::size_t kMaxHistoryEntries = 128;
  // Total-byte ceiling across all undo entries. The 128-entry count cap does not
  // bound bytes: a single select-all replace/sort on a large document copies the
  // whole range into before_lines+after_lines, so 128 such entries could hold many
  // GB. Evict oldest entries past this budget (always keeping at least one).
  static constexpr std::size_t kMaxHistoryBytes = 256u << 20;  // 256 MiB

  std::deque<Entry> undo_stack_;
  std::deque<Entry> redo_stack_;
  std::vector<UndoGroupFrame> group_stack_;
  // Open typing/deletion run that the next contiguous edit may merge into.
  CoalesceKind active_run_kind_ = CoalesceKind::None;
  bool active_run_last_space_ = false;
};

}  // namespace microide::editor
