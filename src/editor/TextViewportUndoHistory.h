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
// plus the static TryMerge / ReconstructFallback / ApplyEntryToLines helpers).
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

  struct Entry {
    std::size_t start_line = 0;
    std::vector<std::string> before_lines;
    std::vector<std::string> after_lines;
    ViewState before_state;
    ViewState after_state;
    // Cached content byte size, stamped when the entry enters the undo stack so
    // the byte-budget trim stays O(entries). Meaningful only while stacked.
    std::size_t byte_size = 0;
  };

  // Grouping ------------------------------------------------------------
  bool IsGroupActive() const { return !group_stack_.empty(); }
  void BeginGroup(ViewState before_state);
  // Records a candidate child edit. While a group is active this either
  // merges into the aggregate entry or falls back to whole-buffer-snapshot
  // mode; with no group active it either coalesces into the top undo entry
  // (per `hint`) or pushes a fresh entry, clearing the redo stack either way
  // (same base semantics as the prior PushHistoryEntry).
  void RecordEntry(Entry entry, const TextBuffer& current_lines,
                   CoalesceHint hint = CoalesceHint{});
  // Bypass-grouping push (matches PushHistoryEntryDirect): clears redo,
  // pushes onto undo, enforces the history cap. Used by paths that have
  // already built a known-good aggregate (e.g. ResetState / Save).
  void RecordEntryDirect(Entry entry);
  // Closes the innermost group. Returns the aggregate Entry the caller
  // should push onto the undo stack via RecordEntryDirect (or nullopt if
  // the group ended up as a no-op).
  std::optional<Entry> FinishActiveGroup(const TextBuffer& current_lines,
                                          ViewState after_state);

  // Undo / redo stack access -------------------------------------------
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

  // End any open typing/deletion coalesce run so the next edit starts a fresh
  // undo entry. Called when the user explicitly moves the caret (arrow keys,
  // Home/End): without it, typing `a`, moving away and back to the same column,
  // then typing `b` would merge `a`+`b` into one undo step.
  void NotifyCursorMoved() { EndCoalesceRun(); }

  // Pure helpers -------------------------------------------------------
  static void ApplyEntryToLines(std::vector<std::string>& lines, const Entry& entry,
                                 bool forward);
  // Same as ApplyEntryToLines but mutating the document's TextBuffer in place
  // through its splice primitive. Used by the live undo/redo apply path.
  static void ApplyEntryToBuffer(TextBuffer& lines, const Entry& entry, bool forward);
  static std::optional<AppliedEdit> BuildAppliedEdit(const Entry& entry, bool forward);
  static Entry BuildEntryForDocumentChange(const std::vector<std::string>& before_lines,
                                            const ViewState& before_state,
                                            const std::vector<std::string>& after_lines,
                                            const ViewState& after_state);

 private:
  struct UndoGroupFrame {
    ViewState state;
    std::optional<Entry> aggregate_entry;
    bool using_fallback = false;
    std::vector<std::string> fallback_lines;
  };

  static std::optional<Entry> TryMergeGroupEntry(const Entry& aggregate, const Entry& next);
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
