#include "editor/TextViewportUndoHistory.h"

#include <algorithm>
#include <utility>

#include "editor/TextViewport.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

std::size_t EntryContentBytes(const TextViewportUndoHistory::Entry& entry) {
  std::size_t bytes = 0;
  for (const std::string& line : entry.before_lines) {
    bytes += line.size() + 1;
  }
  for (const std::string& line : entry.after_lines) {
    bytes += line.size() + 1;
  }
  return bytes;
}

}  // namespace

void TextViewportUndoHistory::AppendUndoEntry(Entry entry) {
  entry.byte_size = EntryContentBytes(entry);
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
    std::optional<Entry> merged = TryMergeGroupEntry(entries[*container], next);
    if (merged.has_value()) {
      entries[*container] = std::move(*merged);
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
      std::optional<Entry> merged = TryMergeGroupEntry(entries[i], entries[i + 1]);
      if (merged.has_value()) {
        entries[i] = std::move(*merged);
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        continue;
      }
    }
    ++i;
  }
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
  std::optional<Entry> merged = TryMergeGroupEntry(top, next);
  if (!merged.has_value()) {
    return false;
  }
  // Preserve the run's original start state; advance its end to the new caret.
  merged->before_state = top.before_state;
  merged->after_state = next.after_state;
  top = std::move(*merged);
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
TextViewportUndoHistory::FinishActiveGroup(const TextBuffer& current_lines,
                                           ViewState after_state) {
  if (group_stack_.empty()) {
    return std::nullopt;
  }
  UndoGroupFrame frame = std::move(group_stack_.back());
  group_stack_.pop_back();

  std::vector<Entry>& parts = frame.disjoint_entries;
  if (parts.empty()) {
    return std::nullopt;
  }

  Entry agg;
  if (parts.size() == 1) {
    agg = std::move(parts.front());  // frame is local + popped; safe to move out
  } else {
    // Stitch the disjoint ranges into one contiguous undo entry. The untouched gap
    // lines between consecutive ranges are read from the current buffer (also their
    // pre-group content, since no child edited them) — bounded by the touched span,
    // never a whole-buffer snapshot.
    agg.start_line = parts.front().start_line;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      Entry& part = parts[i];
      const std::size_t part_after_end = part.start_line + part.after_lines.size();
      agg.before_lines.insert(agg.before_lines.end(),
                              std::make_move_iterator(part.before_lines.begin()),
                              std::make_move_iterator(part.before_lines.end()));
      agg.after_lines.insert(agg.after_lines.end(),
                             std::make_move_iterator(part.after_lines.begin()),
                             std::make_move_iterator(part.after_lines.end()));
      if (i + 1 < parts.size()) {
        const std::size_t gap_end = parts[i + 1].start_line;
        for (std::size_t ln = part_after_end; ln < gap_end && ln < current_lines.size(); ++ln) {
          std::string gap_line(current_lines.LineView(ln));
          agg.before_lines.push_back(gap_line);
          agg.after_lines.push_back(std::move(gap_line));
        }
      }
    }
  }
  agg.before_state = frame.state;
  agg.after_state = std::move(after_state);

  if (agg.before_lines.empty() && agg.after_lines.empty()) {
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
  const std::size_t start_line = std::min(entry.start_line, lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;

  const bool same_count_replacement = removed_count > 0 && removed_count == inserted_lines.size() &&
                                      start_line + removed_count <= lines.size();
  if (same_count_replacement) {
    for (std::size_t i = 0; i < removed_count; ++i) {
      lines.SetLine(start_line + i, inserted_lines[i]);
    }
  } else {
    lines.ReplaceLineRange(start_line, removed_count, inserted_lines);
  }
  if (lines.empty()) {
    lines.PushBackLine("");
  }
}

std::optional<AppliedEdit> TextViewportUndoHistory::BuildAppliedEdit(const Entry& entry,
                                                                     bool forward) {
  const std::vector<std::string>& before_lines = forward ? entry.before_lines : entry.after_lines;
  const std::vector<std::string>& after_lines = forward ? entry.after_lines : entry.before_lines;
  if (before_lines.empty() || after_lines.empty()) {
    return std::nullopt;
  }

  const std::string& before_first = before_lines.front();
  const std::string& after_first = after_lines.front();
  std::size_t common_prefix = 0;
  const std::size_t max_prefix = std::min(before_first.size(), after_first.size());
  while (common_prefix < max_prefix && before_first[common_prefix] == after_first[common_prefix]) {
    ++common_prefix;
  }

  const std::string& before_last = before_lines.back();
  const std::string& after_last = after_lines.back();
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

  std::vector<std::string> replacement_lines = after_lines;
  replacement_lines.front().erase(0, common_prefix);
  if (common_suffix > 0) {
    replacement_lines.back().erase(replacement_lines.back().size() - common_suffix);
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
      .replacement_text = util::SerializeLines(replacement_lines, util::LineEnding::LF),
  };
}

std::optional<AppliedEditLineSpan> TextViewportUndoHistory::BuildAppliedEditLineSpan(
    const Entry& entry, bool forward) {
  const std::vector<std::string>& before = forward ? entry.before_lines : entry.after_lines;
  const std::vector<std::string>& after = forward ? entry.after_lines : entry.before_lines;

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
    const std::vector<std::string>& before_lines, const ViewState& before_state,
    const std::vector<std::string>& after_lines, const ViewState& after_state) {
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
  entry.before_lines.assign(before_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                            before_lines.begin() + static_cast<std::ptrdiff_t>(before_end));
  entry.after_lines.assign(after_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                           after_lines.begin() + static_cast<std::ptrdiff_t>(after_end));
  entry.before_state = before_state;
  entry.after_state = after_state;
  return entry;
}

std::optional<TextViewportUndoHistory::Entry>
TextViewportUndoHistory::TryMergeGroupEntry(const Entry& aggregate, const Entry& next) {
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  Entry merged = aggregate;
  merged.after_state = next.after_state;

  if (next_end == aggregate_after_start) {
    merged.start_line = next.start_line;
    merged.before_lines = next.before_lines;
    merged.before_lines.insert(merged.before_lines.end(), aggregate.before_lines.begin(),
                               aggregate.before_lines.end());
    merged.after_lines = next.after_lines;
    merged.after_lines.insert(merged.after_lines.end(), aggregate.after_lines.begin(),
                              aggregate.after_lines.end());
    return merged;
  }

  if (next_start == aggregate_after_end) {
    merged.before_lines.insert(merged.before_lines.end(), next.before_lines.begin(),
                               next.before_lines.end());
    merged.after_lines.insert(merged.after_lines.end(), next.after_lines.begin(),
                              next.after_lines.end());
    return merged;
  }

  if (next_start < aggregate_after_start || next_end > aggregate_after_end) {
    return std::nullopt;
  }

  const std::size_t relative_start = next_start - aggregate_after_start;
  const std::size_t relative_end = relative_start + next.before_lines.size();
  if (!std::equal(next.before_lines.begin(), next.before_lines.end(),
                  aggregate.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
                  aggregate.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_end))) {
    return std::nullopt;
  }

  merged.after_lines.erase(merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
                            merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_end));
  merged.after_lines.insert(merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
                             next.after_lines.begin(), next.after_lines.end());
  return merged;
}

}  // namespace microide::editor
