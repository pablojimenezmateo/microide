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

void TextViewportUndoHistory::RecordEntry(Entry entry,
                                          const TextBuffer& current_lines,
                                          CoalesceHint hint) {
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

  // A grouped edit owns its own aggregation; never coalesce across it. The
  // aggregate entry is the single source of truth per frame: merge-clean edits
  // fold into it, and a merge conflict reconstructs the pre-group buffer from it
  // rather than from a retained per-child copy. Keeping only the aggregate (not
  // a `child_entries` list) matters on the hot path: a grouped line-move over a
  // wide multi-caret range records one entry holding hundreds of line slices, so
  // an extra deep copy per edit is what regressed `editor_shaping_multi_caret`.
  active_run_kind_ = CoalesceKind::None;
  const std::size_t frame_count = group_stack_.size();
  for (std::size_t fi = 0; fi < frame_count; ++fi) {
    UndoGroupFrame& frame = group_stack_[fi];
    // Only the innermost (last) frame may consume `entry` by move: outer frames
    // are processed first and still read it; after the loop `entry` is dead.
    const bool can_move = (fi + 1 == frame_count);
    if (frame.using_fallback) {
      continue;  // fallback mode captures the final buffer snapshot at finish
    }
    if (!frame.aggregate_entry.has_value()) {
      frame.aggregate_entry = can_move ? std::move(entry) : entry;
      continue;
    }
    std::optional<Entry> merged = TryMergeGroupEntry(*frame.aggregate_entry, entry);
    if (merged.has_value()) {
      frame.aggregate_entry = std::move(merged);
      continue;
    }
    // Merge conflict: drop to whole-buffer-snapshot mode. Reconstruct the
    // pre-group document by reverse-applying the failing entry and then the
    // aggregate-so-far onto the current buffer. This equals reverse-applying
    // every child edit in turn (the aggregate is exactly their contiguous
    // merge), but without retaining a per-entry copy of each grouped edit.
    std::vector<std::string> reconstructed = current_lines.Snapshot();
    ApplyEntryToLines(reconstructed, entry, /*forward=*/false);
    ApplyEntryToLines(reconstructed, *frame.aggregate_entry, /*forward=*/false);
    frame.fallback_lines = std::move(reconstructed);
    frame.aggregate_entry.reset();
    frame.using_fallback = true;
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

  Entry agg;
  if (frame.using_fallback) {
    agg = BuildEntryForDocumentChange(frame.fallback_lines, frame.state, current_lines.Snapshot(),
                                      std::move(after_state));
  } else if (frame.aggregate_entry.has_value()) {
    agg = std::move(*frame.aggregate_entry);  // frame is local + popped; safe to move out
    agg.before_state = frame.state;
    agg.after_state = std::move(after_state);
  } else {
    agg = BuildEntryForDocumentChange({}, frame.state, {}, std::move(after_state));
  }

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
void TextViewportUndoHistory::ApplyEntryToLines(std::vector<std::string>& lines,
                                                const Entry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;

  const bool same_count_replacement = removed_count > 0 && removed_count == inserted_lines.size() &&
                                      start_line + removed_count <= lines.size();
  if (same_count_replacement) {
    for (std::size_t i = 0; i < removed_count; ++i) {
      lines[start_line + i] = inserted_lines[i];
    }
  } else {
    const auto erase_begin = lines.begin() + static_cast<std::ptrdiff_t>(start_line);
    const auto erase_end =
        erase_begin + static_cast<std::ptrdiff_t>(std::min(removed_count, lines.size() - start_line));
    lines.erase(erase_begin, erase_end);
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start_line),
                 inserted_lines.begin(), inserted_lines.end());
  }
  if (lines.empty()) {
    lines.push_back("");
  }
}

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
