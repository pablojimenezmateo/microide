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

}

void TextViewportUndoHistory::AppendUndoEntry(Entry entry) {
  entry.byte_size = EntryContentBytes(entry);
  entry.serial = next_serial_++;
  if (entry.is_multi_range()) {
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
    ++base_epoch_;
  }
  std::size_t total = 0;
  for (const Entry& entry : undo_stack_) {
    total += entry.byte_size;
  }
  while (undo_stack_.size() > 1 && total > kMaxHistoryBytes) {
    total -= undo_stack_.front().byte_size;
    undo_stack_.pop_front();
    ++base_epoch_;
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

  active_run_kind_ = CoalesceKind::None;
  const std::size_t frame_count = group_stack_.size();
  for (std::size_t fi = 0; fi < frame_count; ++fi) {
    UndoGroupFrame& frame = group_stack_[fi];
    const bool can_move = (fi + 1 == frame_count);
    // NOLINTNEXTLINE(bugprone-use-after-move): moved on the last iteration only.
    MergeChildIntoDisjoint(frame.disjoint_entries, can_move ? std::move(entry) : entry);
  }
}

void TextViewportUndoHistory::MergeChildIntoDisjoint(std::vector<Entry>& entries, Entry next) {
  const std::size_t child_start = next.start_line;
  const std::size_t child_before_end = child_start + next.before_lines.size();
  const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(next.after_lines.size()) -
                               static_cast<std::ptrdiff_t>(next.before_lines.size());

  std::optional<std::size_t> container;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::size_t after_start = entries[i].start_line;
    const std::size_t after_end = after_start + entries[i].after_lines.size();
    if (after_start <= child_start && child_before_end <= after_end) {
      container = i;
      break;
    }
  }

  std::size_t overlap_first = entries.size();
  std::size_t overlap_last = entries.size();
  if (!container.has_value()) {
    const auto overlaps = [&](const Entry& e) {
      const std::size_t start = e.start_line;
      const std::size_t end = start + e.after_lines.size();
      return start == end ? child_start < start && start < child_before_end
                          : start < child_before_end && child_start < end;
    };
    const auto first = std::find_if(entries.begin(), entries.end(), overlaps);
    overlap_first = static_cast<std::size_t>(first - entries.begin());
    overlap_last = static_cast<std::size_t>(std::find_if_not(first, entries.end(), overlaps) - entries.begin());
  }

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
  } else if (overlap_first == overlap_last) {
    InsertSortedDisjoint(entries, std::move(next));
  } else {
    MergeChildOverRanges(entries, overlap_first, overlap_last, std::move(next));
  }
  CoalesceAdjacentDisjoint(entries);
}

void TextViewportUndoHistory::MergeChildOverRanges(std::vector<Entry>& entries,
                                                   std::size_t first_index,
                                                   std::size_t last_index, Entry next) {
  const std::size_t child_start = next.start_line;
  const std::size_t child_end = child_start + next.before_lines.size();
  const auto first = entries.begin() + static_cast<std::ptrdiff_t>(first_index);
  const auto last = entries.begin() + static_cast<std::ptrdiff_t>(last_index);
  const Entry& low = *first;
  const Entry& high = *(last - 1);

  LineBlob before;
  std::size_t at = std::min(child_start, low.start_line);
  for (auto it = first; it != last; ++it) {
    if (at < it->start_line) {
      before.append(next.before_lines.SubRange(at - child_start, it->start_line - child_start));
    }
    before.append(it->before_lines);
    at = it->start_line + it->after_lines.size();
  }
  if (at < child_end) {
    before.append(next.before_lines.SubRange(at - child_start, child_end - child_start));
  }

  LineBlob after;
  if (low.start_line < child_start) {
    after.append(low.after_lines.SubRange(0, child_start - low.start_line));
  }
  after.append(next.after_lines);
  const std::size_t high_end = high.start_line + high.after_lines.size();
  if (child_end < high_end) {
    after.append(high.after_lines.SubRange(child_end - high.start_line, high.after_lines.size()));
  }

  Entry merged = std::move(next);
  merged.start_line = std::min(child_start, low.start_line);
  merged.before_state = low.before_state;
  merged.before_lines = std::move(before);
  merged.after_lines = std::move(after);
  const auto slot = entries.erase(first, last);
  entries.insert(slot, std::move(merged));
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
        const bool high_side_is_bigger =
            entries[i + 1].before_lines.content_bytes() +
                entries[i + 1].after_lines.content_bytes() >
            entries[i].before_lines.content_bytes() + entries[i].after_lines.content_bytes();
        if (high_side_is_bigger && CanMergeGroupEntry(entries[i + 1], entries[i])) {
          ViewState after_state = entries[i + 1].after_state;
          entries[i] = MergeGroupEntry(std::move(entries[i + 1]), entries[i]);
          entries[i].after_state = std::move(after_state);
        } else {
          entries[i] = MergeGroupEntry(std::move(entries[i]), entries[i + 1]);
        }
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
    return true;
  }
  if (next_start < aggregate_start || next_end > aggregate_end) {
    return false;
  }
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
    aggregate.removed_text.append(next.removed_text);
    aggregate.inserted_text.append(next.inserted_text);
  } else if (next_end == aggregate_start) {
    aggregate.start_column = next_start;
    aggregate.removed_text.insert(0, next.removed_text);
    aggregate.inserted_text.insert(0, next.inserted_text);
  } else {
    aggregate.inserted_text.replace(next_start - aggregate_start, next.removed_text.size(),
                                    next.inserted_text);
  }
  aggregate.after_state = next.after_state;
}

bool TextViewportUndoHistory::TryCoalesceWithTop(const Entry& next, CoalesceHint hint) {
  if (active_run_kind_ != hint.kind || undo_stack_.empty()) {
    return false;
  }
  if (!hint.changed_is_space && active_run_last_space_) {
    return false;
  }
  if (next.before_state.selection_anchor.has_value()) {
    return false;
  }
  Entry& top = undo_stack_.back();
  if (next.before_state.cursor_line != top.after_state.cursor_line ||
      next.before_state.cursor_column != top.after_state.cursor_column) {
    return false;
  }
  if (top.is_inline != next.is_inline) {
    return false;
  }
  if (top.is_multi_range() || next.is_multi_range()) {
    if (!CanMergeSameShapeMultiRange(top, next)) {
      return false;
    }
    MergeSameShapeMultiRange(top, next);
    top.byte_size = EntryContentBytes(top);
    active_run_last_space_ = hint.changed_is_space;
    EnforceHistoryBudget();
    return true;
  }
  if (top.is_inline) {
    if (!CanMergeInlineEntry(top, next)) {
      return false;
    }
    MergeInlineEntryInto(top, next);
    top.byte_size = EntryContentBytes(top);
    active_run_last_space_ = hint.changed_is_space;
    EnforceHistoryBudget();
    return true;
  }
  if (!CanMergeGroupEntry(top, next)) {
    return false;
  }
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

  Entry agg = std::move(parts.front());
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

void TextViewportUndoHistory::MarkNoPositionSaved() {
  for (Entry& entry : undo_stack_) {
    entry.before_state.dirty = true;
    entry.after_state.dirty = true;
  }
  for (Entry& entry : redo_stack_) {
    entry.before_state.dirty = true;
    entry.after_state.dirty = true;
  }
}

bool TextViewportUndoHistory::HasSavedPosition() const {
  const auto clean = [](const Entry& entry) {
    return !entry.before_state.dirty || !entry.after_state.dirty;
  };
  return std::any_of(undo_stack_.begin(), undo_stack_.end(), clean) ||
         std::any_of(redo_stack_.begin(), redo_stack_.end(), clean);
}

void TextViewportUndoHistory::MarkSaved() {
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
  EndCoalesceRun();
}

TextViewportUndoHistory::SavePoint TextViewportUndoHistory::CaptureSavePoint() {
  EndCoalesceRun();
  return SavePoint{.serial = undo_stack_.empty() ? 0 : undo_stack_.back().serial, .base_epoch = base_epoch_};
}

void TextViewportUndoHistory::MarkSavedAt(const SavePoint& point) {
  MarkNoPositionSaved();
  std::vector<Entry*> timeline;
  timeline.reserve(undo_stack_.size() + redo_stack_.size());
  for (Entry& entry : undo_stack_) {
    timeline.push_back(&entry);
  }
  for (auto it = redo_stack_.rbegin(); it != redo_stack_.rend(); ++it) {
    timeline.push_back(&*it);
  }
  if (point.serial == 0) {
    if (point.base_epoch == base_epoch_ && !timeline.empty()) {
      timeline.front()->before_state.dirty = false;
    }
    return;
  }
  const auto saved = std::find_if(timeline.begin(), timeline.end(),
                                  [&point](const Entry* entry) { return entry->serial == point.serial; });
  if (saved == timeline.end()) {
    return;
  }
  (*saved)->after_state.dirty = false;
  if (std::next(saved) != timeline.end()) {
    (*std::next(saved))->before_state.dirty = false;
  }
}

bool TextViewportUndoHistory::IsAtSavePoint(const SavePoint& point) const {
  const std::uint64_t top = undo_stack_.empty() ? 0 : undo_stack_.back().serial;
  return top == point.serial && (point.serial != 0 || point.base_epoch == base_epoch_);
}

void TextViewportUndoHistory::Clear() {
  EndCoalesceRun();
  ++base_epoch_;
  undo_stack_.clear();
  redo_stack_.clear();
  group_stack_.clear();
}

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

  lines.ReplaceLineRange(start_line, removed_count, inserted_lines);
  if (lines.empty()) {
    lines.PushBackLine("");
  }
}

std::optional<AppliedEdit> TextViewportUndoHistory::BuildAppliedEdit(const Entry& entry,
                                                                     bool forward) {
  if (entry.is_multi_range()) {
    return std::nullopt;
  }
  if (entry.is_inline) {
    const std::string& removed = forward ? entry.removed_text : entry.inserted_text;
    const std::string& inserted = forward ? entry.inserted_text : entry.removed_text;
    if (removed.empty() && inserted.empty()) {
      return std::nullopt;
    }
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

  std::string replacement_text;
  {
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
  if (entry.is_multi_range()) {
    return std::nullopt;
  }
  if (entry.is_inline) {
    if (entry.removed_text == entry.inserted_text) {
      return std::nullopt;
    }
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
    return std::nullopt;
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
  entry.before_lines = before_lines.SubRange(prefix, before_end);
  entry.after_lines = after_lines.SubRange(prefix, after_end);
  entry.before_state = before_state;
  entry.after_state = after_state;
  return entry;
}

bool TextViewportUndoHistory::CanMergeSameShapeMultiRange(const Entry& top, const Entry& next) {
  if (top.is_inline || next.is_inline || !top.is_multi_range() ||
      top.extra_parts.size() != next.extra_parts.size() || top.start_line != next.start_line ||
      top.after_lines.size() != next.before_lines.size() ||
      !top.after_lines.range_equals(0, next.before_lines)) {
    return false;
  }
  for (std::size_t i = 0; i < top.extra_parts.size(); ++i) {
    const DisjointPart& installed = top.extra_parts[i];
    const DisjointPart& replaced = next.extra_parts[i];
    if (installed.after_start != replaced.before_start ||
        installed.after_lines.size() != replaced.before_lines.size() ||
        !installed.after_lines.range_equals(0, replaced.before_lines)) {
      return false;
    }
  }
  return true;
}

void TextViewportUndoHistory::MergeSameShapeMultiRange(Entry& top, const Entry& next) {
  top.after_lines = next.after_lines;
  for (std::size_t i = 0; i < top.extra_parts.size(); ++i) {
    top.extra_parts[i].after_start = next.extra_parts[i].after_start;
    top.extra_parts[i].after_lines = next.extra_parts[i].after_lines;
  }
  top.after_state = next.after_state;
}

bool TextViewportUndoHistory::CanMergeGroupEntry(const Entry& aggregate, const Entry& next) {
  if (aggregate.is_inline || next.is_inline) {
    return false;
  }
  if (aggregate.is_multi_range() || next.is_multi_range()) {
    return false;
  }
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  if (next_end == aggregate_after_start || next_start == aggregate_after_end) {
    return true;
  }
  if (next_start < aggregate_after_start || next_end > aggregate_after_end) {
    return false;
  }
  const std::size_t relative_start = next_start - aggregate_after_start;
  return aggregate.after_lines.range_equals(relative_start, next.before_lines);
}

TextViewportUndoHistory::Entry TextViewportUndoHistory::MergeGroupEntry(Entry aggregate,
                                                                       const Entry& next) {
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  Entry merged = std::move(aggregate);
  merged.after_state = next.after_state;

  if (next_end == aggregate_after_start) {
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

}

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

}
