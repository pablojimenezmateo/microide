#include "editor/TextViewportUndoHistory.h"

#include <algorithm>
#include <utility>

#include "editor/TextViewport.h"
#include "util/StringUtil.h"

namespace microide::editor {

void TextViewportUndoHistory::BeginGroup(ViewState before_state) {
  UndoGroupFrame frame;
  frame.state = std::move(before_state);
  group_stack_.push_back(std::move(frame));
}

void TextViewportUndoHistory::RecordEntry(Entry entry,
                                          const std::vector<std::string>& current_lines) {
  redo_stack_.clear();
  if (group_stack_.empty()) {
    undo_stack_.push_back(std::move(entry));
    if (undo_stack_.size() > kMaxHistoryEntries) {
      undo_stack_.pop_front();
    }
    return;
  }

  for (UndoGroupFrame& frame : group_stack_) {
    if (frame.using_fallback) {
      continue;
    }
    frame.child_entries.push_back(entry);
    if (!frame.aggregate_entry.has_value()) {
      frame.aggregate_entry = entry;
      continue;
    }
    std::optional<Entry> merged = TryMergeGroupEntry(*frame.aggregate_entry, entry);
    if (merged.has_value()) {
      frame.aggregate_entry = std::move(merged);
      continue;
    }
    frame.fallback_lines = ReconstructFallbackLines(current_lines, frame.child_entries);
    frame.aggregate_entry.reset();
    frame.child_entries.clear();
    frame.using_fallback = true;
  }
}

void TextViewportUndoHistory::RecordEntryDirect(Entry entry) {
  redo_stack_.clear();
  undo_stack_.push_back(std::move(entry));
  if (undo_stack_.size() > kMaxHistoryEntries) {
    undo_stack_.pop_front();
  }
}

std::optional<TextViewportUndoHistory::Entry>
TextViewportUndoHistory::FinishActiveGroup(const std::vector<std::string>& current_lines,
                                           ViewState after_state) {
  if (group_stack_.empty()) {
    return std::nullopt;
  }
  UndoGroupFrame frame = std::move(group_stack_.back());
  group_stack_.pop_back();

  Entry agg;
  if (frame.using_fallback) {
    agg = BuildEntryForDocumentChange(frame.fallback_lines, frame.state, current_lines,
                                      std::move(after_state));
  } else if (frame.aggregate_entry.has_value()) {
    agg = *frame.aggregate_entry;
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
  Entry entry = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  return entry;
}

TextViewportUndoHistory::Entry TextViewportUndoHistory::PopRedo() {
  Entry entry = std::move(redo_stack_.back());
  redo_stack_.pop_back();
  return entry;
}

void TextViewportUndoHistory::PushUndo(Entry entry) {
  undo_stack_.push_back(std::move(entry));
  if (undo_stack_.size() > kMaxHistoryEntries) {
    undo_stack_.pop_front();
  }
}

void TextViewportUndoHistory::PushRedo(Entry entry) {
  redo_stack_.push_back(std::move(entry));
}

void TextViewportUndoHistory::Clear() {
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

std::vector<std::string> TextViewportUndoHistory::ReconstructFallbackLines(
    const std::vector<std::string>& current_lines, const std::vector<Entry>& child_entries) {
  std::vector<std::string> reconstructed = current_lines;
  for (auto it = child_entries.rbegin(); it != child_entries.rend(); ++it) {
    ApplyEntryToLines(reconstructed, *it, /*forward=*/false);
  }
  return reconstructed;
}

}  // namespace microide::editor
