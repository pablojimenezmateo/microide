#include "editor/TextViewportUndoHistory.h"

#include <algorithm>
#include <utility>

#include "editor/TextViewport.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

std::size_t EntryContentBytes(const TextViewportUndoHistory::Entry& entry) {
  if (entry.is_inline) {
    return entry.removed_text.size() + entry.inserted_text.size();
  }
  std::size_t bytes = 0;
  const auto add_lines = [&bytes](const LineBlob& lines) {
    // Content bytes plus one separator per line -- the same number the vector
    // form summed, so the byte budget is unchanged by the storage change.
    bytes += lines.content_bytes() + lines.size();
  };
  add_lines(entry.before_lines);
  add_lines(entry.after_lines);
  for (const auto& part : entry.extra_parts) {
    add_lines(part.before_lines);
    add_lines(part.after_lines);
  }
  return bytes;
}

}  // namespace

void TextViewportUndoHistory::AppendUndoEntry(Entry entry) {
  entry.byte_size = EntryContentBytes(entry);
  if (entry.is_multi_range()) {
    // What the entry holds vs what a single contiguous range would have held for
    // the same edit — the span/edited ratio TD-2026-08-06-157 asked for, which
    // nothing reported. `lines_spanned` counts both images, matching `lines_kept`.
    std::size_t kept = entry.before_lines.size() + entry.after_lines.size();
    std::size_t highest_before_end = entry.start_line + entry.before_lines.size();
    std::size_t highest_after_end = entry.start_line + entry.after_lines.size();
    for (const DisjointPart& part : entry.extra_parts) {
      kept += part.before_lines.size() + part.after_lines.size();
      highest_before_end = std::max(highest_before_end, part.before_start + part.before_lines.size());
      highest_after_end = std::max(highest_after_end, part.after_start + part.after_lines.size());
    }
    util::AddPerformanceCounter(util::PerfCounterId::EditorMultiRangeUndoEntries, 1);
    util::AddPerformanceCounter(util::PerfCounterId::EditorMultiRangeUndoLinesKept, kept);
    util::AddPerformanceCounter(
        util::PerfCounterId::EditorMultiRangeUndoLinesSpanned,
        (highest_before_end - entry.start_line) + (highest_after_end - entry.start_line));
  }
  undo_stack_.push_back(std::move(entry));
  EnforceHistoryBudget();
}

void TextViewportUndoHistory::EnforceHistoryBudget() {
  while (undo_stack_.size() > kMaxHistoryEntries) {
    undo_stack_.pop_front();
  }
  std::size_t total = 0;
  for (const Entry& entry : undo_stack_) {
    total += entry.byte_size;
  }
  // Keep at least the most recent entry even if it alone exceeds the budget —
  // dropping it would silently discard the just-made edit's undo.
  while (undo_stack_.size() > 1 && total > kMaxHistoryBytes) {
    total -= undo_stack_.front().byte_size;
    undo_stack_.pop_front();
  }
}

void TextViewportUndoHistory::BeginGroup(ViewState before_state) {
  UndoGroupFrame frame;
  frame.state = std::move(before_state);
  group_stack_.push_back(std::move(frame));
}

void TextViewportUndoHistory::RecordEntry(Entry entry, CoalesceHint hint) {
  redo_stack_.clear();
  if (group_stack_.empty()) {
    if (hint.kind != CoalesceKind::None && TryCoalesceWithTop(entry, hint)) {
      return;
    }
    AppendUndoEntry(std::move(entry));
    active_run_kind_ = hint.kind;
    active_run_last_space_ = hint.changed_is_space;
    return;
  }

  // A grouped edit owns its own aggregation; never coalesce across it. Each frame
  // keeps a disjoint-range set (see UndoGroupFrame): contiguous children fold into
  // one range, non-contiguous children accumulate as extra ranges. This keeps the
  // wholly-contiguous fast path a single-element vector (matching the old single
  // aggregate — `editor_shaping_multi_caret` stays one merge/deep-copy per child)
  // while never materializing a whole-buffer snapshot for non-contiguous edits.
  active_run_kind_ = CoalesceKind::None;
  const std::size_t frame_count = group_stack_.size();
  for (std::size_t fi = 0; fi < frame_count; ++fi) {
    UndoGroupFrame& frame = group_stack_[fi];
    // Only the innermost (last) frame may consume `entry` by move: outer frames
    // are processed first and still read it; after the loop `entry` is dead.
    const bool can_move = (fi + 1 == frame_count);
    MergeChildIntoDisjoint(frame.disjoint_entries, can_move ? std::move(entry) : entry);
  }
}

void TextViewportUndoHistory::MergeChildIntoDisjoint(std::vector<Entry>& entries, Entry next) {
  const std::size_t child_start = next.start_line;
  const std::size_t child_before_end = child_start + next.before_lines.size();
  const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(next.after_lines.size()) -
                               static_cast<std::ptrdiff_t>(next.before_lines.size());

  // Detect (in pre-shift coordinates) an existing range whose after-content wholly
  // contains the child's replaced span — the child re-edited inside an earlier
  // aggregate. TryMergeGroupEntry splices it in place; no new range is created.
  std::optional<std::size_t> container;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::size_t after_start = entries[i].start_line;
    const std::size_t after_end = after_start + entries[i].after_lines.size();
    if (after_start <= child_start && child_before_end <= after_end) {
      container = i;
      break;
    }
  }

  // Shift every range strictly below the child's replaced span into the post-edit
  // coordinate frame. The container (start < child_before_end) is never in this set;
  // a right neighbour that sat exactly at child_before_end shifts to abut the child.
  if (delta != 0) {
    for (Entry& e : entries) {
      if (e.start_line >= child_before_end) {
        e.start_line =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(e.start_line) + delta);
      }
    }
  }

  if (container.has_value()) {
    if (CanMergeGroupEntry(entries[*container], next)) {
      entries[*container] = MergeGroupEntry(std::move(entries[*container]), next);
    } else {
      InsertSortedDisjoint(entries, std::move(next));
    }
  } else {
    InsertSortedDisjoint(entries, std::move(next));
  }
  CoalesceAdjacentDisjoint(entries);
}

void TextViewportUndoHistory::InsertSortedDisjoint(std::vector<Entry>& entries, Entry next) {
  const auto pos = std::lower_bound(
      entries.begin(), entries.end(), next.start_line,
      [](const Entry& e, std::size_t value) { return e.start_line < value; });
  entries.insert(pos, std::move(next));
}

void TextViewportUndoHistory::CoalesceAdjacentDisjoint(std::vector<Entry>& entries) {
  for (std::size_t i = 0; i + 1 < entries.size();) {
    const std::size_t prev_after_end = entries[i].start_line + entries[i].after_lines.size();
    if (entries[i + 1].start_line == prev_after_end) {
      if (CanMergeGroupEntry(entries[i], entries[i + 1])) {
        entries[i] = MergeGroupEntry(std::move(entries[i]), entries[i + 1]);
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        continue;
      }
    }
    ++i;
  }
}

bool TextViewportUndoHistory::CanMergeInlineEntry(const Entry& aggregate, const Entry& next) {
  if (aggregate.start_line != next.start_line) {
    return false;
  }
  const std::size_t aggregate_start = aggregate.start_column;
  const std::size_t aggregate_end = aggregate_start + aggregate.inserted_text.size();
  const std::size_t next_start = next.start_column;
  const std::size_t next_end = next_start + next.removed_text.size();

  if (next_start == aggregate_end || next_end == aggregate_start) {
    return true;  // abuts on either side
  }
  if (next_start < aggregate_start || next_end > aggregate_end) {
    return false;
  }
  // Contained: the bytes `next` claims to remove must be the ones the aggregate
  // put there, or the two entries disagree about the line's content.
  return std::equal(
      next.removed_text.begin(), next.removed_text.end(),
      aggregate.inserted_text.begin() + static_cast<std::ptrdiff_t>(next_start - aggregate_start));
}

void TextViewportUndoHistory::MergeInlineEntryInto(Entry& aggregate, const Entry& next) {
  const std::size_t aggregate_start = aggregate.start_column;
  const std::size_t aggregate_end = aggregate_start + aggregate.inserted_text.size();
  const std::size_t next_start = next.start_column;
  const std::size_t next_end = next_start + next.removed_text.size();

  if (next_start == aggregate_end) {
    // Appends. In pre-run coordinates `next` removes the bytes immediately after
    // what the aggregate removed, so both deltas simply concatenate.
    aggregate.removed_text.append(next.removed_text);
    aggregate.inserted_text.append(next.inserted_text);
  } else if (next_end == aggregate_start) {
    // Prepends (a backspace run): `next` removes bytes that sit before the
    // aggregate's edit point and are therefore already in pre-run coordinates.
    aggregate.start_column = next_start;
    aggregate.removed_text.insert(0, next.removed_text);
    aggregate.inserted_text.insert(0, next.inserted_text);
  } else {
    // Contained: splice into the aggregate's inserted text; what the run removed
    // from the ORIGINAL line is unchanged.
    aggregate.inserted_text.replace(next_start - aggregate_start, next.removed_text.size(),
                                    next.inserted_text);
  }
  aggregate.after_state = next.after_state;
}

bool TextViewportUndoHistory::TryCoalesceWithTop(const Entry& next, CoalesceHint hint) {
  if (active_run_kind_ != hint.kind || undo_stack_.empty()) {
    return false;
  }
  // Split before a new word: a non-space char immediately after a space ends
  // the previous run (so "foo " and "bar" undo separately).
  if (!hint.changed_is_space && active_run_last_space_) {
    return false;
  }
  // Replacing a selection is a discrete edit, not a continuation of typing.
  if (next.before_state.selection_anchor.has_value()) {
    return false;
  }
  // The new edit must begin exactly where the run left off; any caret jump
  // (arrow key, click, vertical move) breaks the run.
  Entry& top = undo_stack_.back();
  if (next.before_state.cursor_line != top.after_state.cursor_line ||
      next.before_state.cursor_column != top.after_state.cursor_column) {
    return false;
  }
  // A run never mixes the two entry shapes. In practice it cannot want to: every
  // edit that produces a line-form entry (newline, line join, multi-line paste)
  // carries CoalesceKind::None and so ends the run before it gets here. Refusing
  // the mix costs at most one extra undo step at a line boundary.
  if (top.is_inline != next.is_inline) {
    return false;
  }
  // A multi-range entry is never a typing-run continuation (TD-2026-08-06-157).
  if (top.is_multi_range() || next.is_multi_range()) {
    return false;
  }
  if (top.is_inline) {
    if (!CanMergeInlineEntry(top, next)) {
      return false;
    }
    // Only after_state advances; the run keeps its original start state.
    MergeInlineEntryInto(top, next);
    top.byte_size = EntryContentBytes(top);
    active_run_last_space_ = hint.changed_is_space;
    EnforceHistoryBudget();
    return true;
  }
  if (!CanMergeGroupEntry(top, next)) {
    return false;
  }
  // Preserve the run's original start state; advance its end to the new caret.
  // `before_state` is read off `top` before it is consumed, since MergeGroupEntry
  // takes the aggregate by value.
  const ViewState run_start_state = top.before_state;
  Entry merged = MergeGroupEntry(std::move(top), next);
  merged.before_state = run_start_state;
  merged.after_state = next.after_state;
  top = std::move(merged);
  top.byte_size = EntryContentBytes(top);
  active_run_last_space_ = hint.changed_is_space;
  EnforceHistoryBudget();
  return true;
}

void TextViewportUndoHistory::RecordEntryDirect(Entry entry) {
  redo_stack_.clear();
  EndCoalesceRun();
  AppendUndoEntry(std::move(entry));
}

std::optional<TextViewportUndoHistory::Entry>
TextViewportUndoHistory::FinishActiveGroup(ViewState after_state) {
  if (group_stack_.empty()) {
    return std::nullopt;
  }
  UndoGroupFrame frame = std::move(group_stack_.back());
  group_stack_.pop_back();

  std::vector<Entry>& parts = frame.disjoint_entries;
  if (parts.empty()) {
    return std::nullopt;
  }

  // The frame already carries the edit as a disjoint set, and the entry now keeps
  // it that way. This used to stitch the ranges into one contiguous replacement by
  // re-reading every untouched gap line out of the buffer into BOTH images — so a
  // group whose children sat far apart recorded, and retained, two copies of
  // everything between them (TD-2026-08-06-157).
  //
  // The lowest range is the primary; its start_line is valid in both coordinate
  // frames because nothing below it moves. Each higher range keeps the
  // after-coordinate the frame maintained plus the before-coordinate derived by
  // undoing the net line delta of every range below it.
  Entry agg = std::move(parts.front());  // frame is local + popped; safe to move out
  agg.extra_parts.clear();
  if (parts.size() > 1) {
    agg.extra_parts.reserve(parts.size() - 1);
    std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(agg.after_lines.size()) -
                           static_cast<std::ptrdiff_t>(agg.before_lines.size());
    for (std::size_t i = 1; i < parts.size(); ++i) {
      Entry& part = parts[i];
      const std::size_t after_start = part.start_line;
      DisjointPart extra;
      extra.after_start = after_start;
      extra.before_start = static_cast<std::size_t>(
          std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(after_start) - shift));
      extra.before_lines = std::move(part.before_lines);
      extra.after_lines = std::move(part.after_lines);
      shift += static_cast<std::ptrdiff_t>(extra.after_lines.size()) -
               static_cast<std::ptrdiff_t>(extra.before_lines.size());
      agg.extra_parts.push_back(std::move(extra));
    }
  }
  agg.before_state = frame.state;
  agg.after_state = std::move(after_state);

  if (agg.before_lines.empty() && agg.after_lines.empty() && agg.extra_parts.empty()) {
    return std::nullopt;
  }
  return agg;
}

TextViewportUndoHistory::Entry TextViewportUndoHistory::PopUndo() {
  EndCoalesceRun();
  Entry entry = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  return entry;
}

TextViewportUndoHistory::Entry TextViewportUndoHistory::PopRedo() {
  EndCoalesceRun();
  Entry entry = std::move(redo_stack_.back());
  redo_stack_.pop_back();
  return entry;
}

void TextViewportUndoHistory::PushUndo(Entry entry) {
  EndCoalesceRun();
  AppendUndoEntry(std::move(entry));
}

void TextViewportUndoHistory::PushRedo(Entry entry) {
  EndCoalesceRun();
  redo_stack_.push_back(std::move(entry));
}

void TextViewportUndoHistory::MarkSaved() {
  // Dirty is tracked per undo/redo position via the captured ViewState flags. The
  // freshly-saved buffer is the one clean position; every OTHER reachable position
  // (reached by undoing past the save, or redoing forward) now differs from disk,
  // so its flag must read dirty. Blanket-mark everything dirty, then clear only the
  // ViewState(s) that represent the current (saved) position: the top undo entry's
  // after_state, and — if we saved while sitting above undone edits — the
  // most-recently-undone redo entry's before_state.
  for (Entry& entry : undo_stack_) {
    entry.before_state.dirty = true;
    entry.after_state.dirty = true;
  }
  for (Entry& entry : redo_stack_) {
    entry.before_state.dirty = true;
    entry.after_state.dirty = true;
  }
  if (!undo_stack_.empty()) {
    undo_stack_.back().after_state.dirty = false;
  }
  if (!redo_stack_.empty()) {
    redo_stack_.back().before_state.dirty = false;
  }
  // A subsequent contiguous edit must start a fresh undo entry rather than
  // coalescing into the just-saved top entry (which would rewrite the saved
  // position's after_state back to dirty).
  EndCoalesceRun();
}

void TextViewportUndoHistory::Clear() {
  EndCoalesceRun();
  undo_stack_.clear();
  redo_stack_.clear();
  group_stack_.clear();
}

// Lifted verbatim from TextViewport::ApplyHistoryEntryToLines.

void TextViewportUndoHistory::ApplyEntryToBuffer(TextBuffer& lines, const Entry& entry,
                                                 bool forward) {
  if (entry.is_inline && !lines.empty()) {
    const std::string& removed = forward ? entry.removed_text : entry.inserted_text;
    const std::string& inserted = forward ? entry.inserted_text : entry.removed_text;
    const std::size_t line = std::min(entry.start_line, lines.size() - 1);
    lines.ReplaceTextRange(line, entry.start_column, line,
                           entry.start_column + removed.size(), inserted);
    return;
  }

  // A multi-range entry replaces several disjoint spans. Walk them highest-first
  // (ForEachPartInApplyOrder) so each part's recorded index is still valid when it
  // is reached: the parts still to come sit below and are unmoved by the ones
  // already spliced (TD-2026-08-06-157).
  if (entry.is_multi_range()) {
    entry.ForEachPartInApplyOrder(
        forward, [&lines](std::size_t start, const LineBlob& removed,
                          const LineBlob& inserted) {
          lines.ReplaceLineRange(std::min(start, lines.size()), removed.size(), inserted);
        });
    if (lines.empty()) {
      lines.PushBackLine("");
    }
    return;
  }

  const std::size_t start_line = std::min(entry.start_line, lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;

  // One bulk splice, never a per-line loop. A same-count replacement used to be
  // written line by line, which was the cheap form back when the document WAS a
  // `std::vector<std::string>` and `SetLine` was an assignment. Against the piece
  // tree it is the expensive form: `SetLine` is itself a `ReplaceLineRange`, so an
  // N-line replacement paid N delete+insert splices, N joined-replacement string
  // allocations, N revision bumps and N line-cache wipes to install what one
  // splice installs. Toggle Comment over a 1,000-line selection was 1,000 of them
  // (TD-2026-08-06-159).
  lines.ReplaceLineRange(start_line, removed_count, inserted_lines);
  if (lines.empty()) {
    lines.PushBackLine("");
  }
}

std::optional<AppliedEdit> TextViewportUndoHistory::BuildAppliedEdit(const Entry& entry,
                                                                     bool forward) {
  // See BuildAppliedEditLineSpan: a multi-range entry has no single replaced range.
  if (entry.is_multi_range()) {
    return std::nullopt;
  }
  if (entry.is_inline) {
    const std::string& removed = forward ? entry.removed_text : entry.inserted_text;
    const std::string& inserted = forward ? entry.inserted_text : entry.removed_text;
    if (removed.empty() && inserted.empty()) {
      return std::nullopt;
    }
    // A coalesced run can leave shared bytes at either end (typing over what the
    // run itself inserted), so trim to the true delta exactly as the line form
    // does -- consumers shift markers by this range.
    const std::size_t max_common = std::min(removed.size(), inserted.size());
    std::size_t prefix = 0;
    while (prefix < max_common && removed[prefix] == inserted[prefix]) {
      ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < max_common - prefix &&
           removed[removed.size() - 1 - suffix] == inserted[inserted.size() - 1 - suffix]) {
      ++suffix;
    }
    return AppliedEdit{
        .range_before =
            SelectionRange{
                .start = TextPosition{entry.start_line, entry.start_column + prefix},
                .end = TextPosition{entry.start_line,
                                    entry.start_column + removed.size() - suffix},
            },
        .replacement_text = inserted.substr(prefix, inserted.size() - prefix - suffix),
    };
  }

  const LineBlob& before_lines = forward ? entry.before_lines : entry.after_lines;
  const LineBlob& after_lines = forward ? entry.after_lines : entry.before_lines;
  if (before_lines.empty() || after_lines.empty()) {
    return std::nullopt;
  }

  const std::string_view before_first = before_lines.front();
  const std::string_view after_first = after_lines.front();
  std::size_t common_prefix = 0;
  const std::size_t max_prefix = std::min(before_first.size(), after_first.size());
  while (common_prefix < max_prefix && before_first[common_prefix] == after_first[common_prefix]) {
    ++common_prefix;
  }

  const std::string_view before_last = before_lines.back();
  const std::string_view after_last = after_lines.back();
  std::size_t common_suffix = 0;
  const std::size_t max_suffix = std::min(before_last.size(), after_last.size());
  while (common_suffix < max_suffix &&
         before_last[before_last.size() - 1 - common_suffix] ==
             after_last[after_last.size() - 1 - common_suffix]) {
    if ((before_lines.size() == 1 && common_prefix + common_suffix >= before_first.size()) ||
        (after_lines.size() == 1 && common_prefix + common_suffix >= after_first.size())) {
      break;
    }
    ++common_suffix;
  }

  // Build the replacement text straight from the trimmed views. This used to
  // deep-copy the whole after-lines vector and then erase the common prefix off
  // the front line and the common suffix off the back one -- i.e. it copied
  // everything the trim was about to throw away. For a one-character insert the
  // result is one byte, and on a line with no newlines in it (a minified bundle)
  // the copy was the whole document, per keystroke.
  //
  // The trims apply to the first and last lines respectively, and when there is
  // only ONE line both apply to it. The loop below keeps that single-line case in
  // the same code path rather than a special case: `first` trims the front,
  // `last` trims the back, and for size()==1 the line is both.
  std::string replacement_text;
  {
    // Content bytes plus the '\n' between each pair -- one read off the blob,
    // where the vector form summed one string at a time.
    std::size_t total = after_lines.content_bytes() + after_lines.size() - 1;
    const std::size_t trimmed = common_prefix + common_suffix;
    replacement_text.reserve(total > trimmed ? total - trimmed : 0);
    for (std::size_t i = 0; i < after_lines.size(); ++i) {
      std::string_view piece(after_lines[i]);
      if (i == 0) {
        piece.remove_prefix(std::min(common_prefix, piece.size()));
      }
      if (i + 1 == after_lines.size() && common_suffix > 0) {
        piece.remove_suffix(std::min(common_suffix, piece.size()));
      }
      replacement_text.append(piece);
      if (i + 1 < after_lines.size()) {
        replacement_text.push_back('\n');
      }
    }
  }

  return AppliedEdit{
      .range_before =
          SelectionRange{
              .start =
                  TextPosition{
                      .line = entry.start_line,
                      .column = common_prefix,
                  },
              .end =
                  TextPosition{
                      .line = entry.start_line + before_lines.size() - 1,
                      .column = before_last.size() - common_suffix,
                  },
          },
      .replacement_text = std::move(replacement_text),
  };
}

std::optional<AppliedEditLineSpan> TextViewportUndoHistory::BuildAppliedEditLineSpan(
    const Entry& entry, bool forward) {
  // A multi-range entry is not one span. Publishing the union would drag markers
  // on the untouched lines between its parts to the union's end, which is exactly
  // the miscollapse ApplyMultiCaretEdit already refuses to publish for the same
  // shape; leaving it empty makes single-range consumers (BreakpointStore, LSP
  // diagnostic shifting) take their resync fallback instead (TD-2026-08-06-157).
  if (entry.is_multi_range()) {
    return std::nullopt;
  }
  if (entry.is_inline) {
    if (entry.removed_text == entry.inserted_text) {
      return std::nullopt;  // no content change
    }
    // An in-line splice never changes the line count and touches exactly one line.
    return AppliedEditLineSpan{
        .old_start = entry.start_line,
        .old_end = entry.start_line + 1,
        .new_end = entry.start_line + 1,
    };
  }

  const LineBlob& before = forward ? entry.before_lines : entry.after_lines;
  const LineBlob& after = forward ? entry.after_lines : entry.before_lines;

  std::size_t prefix = 0;
  while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) {
    ++prefix;
  }
  if (prefix == before.size() && prefix == after.size()) {
    return std::nullopt;  // no line-level change
  }

  std::size_t suffix = 0;
  while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
         before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
    ++suffix;
  }

  return AppliedEditLineSpan{
      .old_start = entry.start_line + prefix,
      .old_end = entry.start_line + (before.size() - suffix),
      .new_end = entry.start_line + (after.size() - suffix),
  };
}

TextViewportUndoHistory::Entry TextViewportUndoHistory::BuildEntryForDocumentChange(
    LineBlob before_lines, const ViewState& before_state,
    LineBlob after_lines, const ViewState& after_state) {
  std::size_t prefix = 0;
  while (prefix < before_lines.size() && prefix < after_lines.size() &&
         before_lines[prefix] == after_lines[prefix]) {
    ++prefix;
  }

  std::size_t before_end = before_lines.size();
  std::size_t after_end = after_lines.size();
  while (before_end > prefix && after_end > prefix &&
         before_lines[before_end - 1] == after_lines[after_end - 1]) {
    --before_end;
    --after_end;
  }

  Entry entry;
  entry.start_line = prefix;
  // SubRange copies the trimmed byte span once each, where the vector form moved
  // (before_end - prefix) + (after_end - prefix) individual strings.
  entry.before_lines = before_lines.SubRange(prefix, before_end);
  entry.after_lines = after_lines.SubRange(prefix, after_end);
  entry.before_state = before_state;
  entry.after_state = after_state;
  return entry;
}

bool TextViewportUndoHistory::CanMergeGroupEntry(const Entry& aggregate, const Entry& next) {
  // Line-form only. Grouped children are widened before they reach a frame (see
  // TextViewport::PushHistoryEntry), and the typing run dispatches inline pairs to
  // CanMergeInlineEntry; anything else that lands here would splice line vectors
  // that an inline entry does not have.
  if (aggregate.is_inline || next.is_inline) {
    return false;
  }
  // Single-range only. A multi-range entry (TD-2026-08-06-157) describes several
  // disjoint splices, and every index test below assumes exactly one; refusing the
  // merge costs at most one extra undo step.
  if (aggregate.is_multi_range() || next.is_multi_range()) {
    return false;
  }
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  // Abutting on either side always merges.
  if (next_end == aggregate_after_start || next_start == aggregate_after_end) {
    return true;
  }
  // Otherwise `next` must sit wholly inside the aggregate's after-image, and the
  // lines it claims to replace must be the ones actually there.
  if (next_start < aggregate_after_start || next_end > aggregate_after_end) {
    return false;
  }
  const std::size_t relative_start = next_start - aggregate_after_start;
  return aggregate.after_lines.range_equals(relative_start, next.before_lines);
}

// Precondition: CanMergeGroupEntry(aggregate, next). Split from the check so the
// aggregate can be MOVED in rather than copied: this used to open with
// `Entry merged = aggregate;`, a deep copy of both line vectors, paid even on the
// two paths that then returned nullopt. Typing coalesces on every keystroke, so on
// a file with no line breaks that was two whole-document copies per character.
TextViewportUndoHistory::Entry TextViewportUndoHistory::MergeGroupEntry(Entry aggregate,
                                                                       const Entry& next) {
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  Entry merged = std::move(aggregate);
  merged.after_state = next.after_state;

  if (next_end == aggregate_after_start) {
    // `next` sits immediately BEFORE the aggregate, so it prepends. The previous
    // form built this as `next.<lines>` copied, then appended the aggregate's --
    // which read the aggregate after it had been consumed once this took it by
    // value. Inserting at the front keeps the aggregate's vectors in place and
    // copies only `next`, which is the smaller side by construction.
    merged.start_line = next.start_line;
    merged.before_lines.prepend(next.before_lines);
    merged.after_lines.prepend(next.after_lines);
    return merged;
  }

  if (next_start == aggregate_after_end) {
    merged.before_lines.append(next.before_lines);
    merged.after_lines.append(next.after_lines);
    return merged;
  }

  // Contained: CanMergeGroupEntry already established the bounds and that the
  // replaced lines match, so this is a straight splice.
  const std::size_t relative_start = next_start - aggregate_after_start;
  const std::size_t relative_end = relative_start + next.before_lines.size();
  merged.after_lines.replace_range(relative_start, relative_end, next.after_lines);
  return merged;
}

namespace {

std::size_t ViewStateBytes(const TextViewportUndoHistory::ViewState& state) {
  return state.secondary_carets.capacity() *
         sizeof(TextViewportUndoHistory::SecondaryCaret);
}

std::size_t EntryBytes(const TextViewportUndoHistory::Entry& entry) {
  std::size_t bytes = sizeof(TextViewportUndoHistory::Entry);
  bytes += entry.removed_text.capacity();
  bytes += entry.inserted_text.capacity();
  const auto add_lines = [&bytes](const LineBlob& lines) {
    bytes += lines.ApproximateResidentBytes();
  };
  add_lines(entry.before_lines);
  add_lines(entry.after_lines);
  bytes += entry.extra_parts.capacity() * sizeof(TextViewportUndoHistory::DisjointPart);
  for (const auto& part : entry.extra_parts) {
    add_lines(part.before_lines);
    add_lines(part.after_lines);
  }
  bytes += ViewStateBytes(entry.before_state);
  bytes += ViewStateBytes(entry.after_state);
  return bytes;
}

}  // namespace

std::size_t TextViewportUndoHistory::ApproximateResidentBytes() const {
  std::size_t bytes = 0;
  for (const Entry& entry : undo_stack_) {
    bytes += EntryBytes(entry);
  }
  for (const Entry& entry : redo_stack_) {
    bytes += EntryBytes(entry);
  }
  for (const UndoGroupFrame& frame : group_stack_) {
    for (const Entry& entry : frame.disjoint_entries) {
      bytes += EntryBytes(entry);
    }
  }
  return bytes;
}

}  // namespace microide::editor
