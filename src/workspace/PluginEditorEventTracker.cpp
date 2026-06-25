#include "workspace/PluginEditorEventTracker.h"

#include <algorithm>

namespace microide::workspace {

void PluginEditorEventTracker::SetInterest(plugin::PluginHost::EditorEventInterest interest) {
  interest_ = interest;
}

void PluginEditorEventTracker::Reset() {
  has_baseline_ = false;
  path_.clear();
  change_dirty_ = false;
  cursor_dirty_ = false;
  selection_dirty_ = false;
}

void PluginEditorEventTracker::Sample(const std::filesystem::path& path,
                                      std::uint64_t content_revision,
                                      std::size_t cursor_line,
                                      std::size_t cursor_column,
                                      bool selection_present,
                                      std::size_t selection_start_line,
                                      std::size_t selection_start_column,
                                      std::size_t selection_end_line,
                                      std::size_t selection_end_column,
                                      std::uint64_t now_ms,
                                      std::uint32_t debounce_ms) {
  if (!interest_.any()) {
    return;
  }

  const auto adopt_baseline = [&]() {
    has_baseline_ = true;
    path_ = path;
    last_revision_ = content_revision;
    last_cursor_line_ = cursor_line;
    last_cursor_column_ = cursor_column;
    last_selection_present_ = selection_present;
    last_selection_start_line_ = selection_start_line;
    last_selection_start_column_ = selection_start_column;
    last_selection_end_line_ = selection_end_line;
    last_selection_end_column_ = selection_end_column;
  };

  // First sample, or the active buffer's identity changed: re-baseline silently
  // so switching tabs never fakes a change/cursor/selection event.
  if (!has_baseline_ || path_ != path) {
    adopt_baseline();
    return;
  }

  const std::uint64_t deadline = now_ms + debounce_ms;

  if (interest_.buffer_change && content_revision != last_revision_) {
    // Union the caret line across the burst as a changed-line hint.
    if (!change_dirty_) {
      change_start_line_ = cursor_line;
      change_end_line_ = cursor_line;
    } else {
      change_start_line_ = std::min(change_start_line_, cursor_line);
      change_end_line_ = std::max(change_end_line_, cursor_line);
    }
    change_dirty_ = true;
    change_deadline_ = deadline;
  }

  if (interest_.cursor_move &&
      (cursor_line != last_cursor_line_ || cursor_column != last_cursor_column_)) {
    cursor_dirty_ = true;
    cursor_deadline_ = deadline;
    cursor_line_ = cursor_line;
    cursor_column_ = cursor_column;
  }

  const bool selection_changed =
      selection_present != last_selection_present_ ||
      (selection_present &&
       (selection_start_line != last_selection_start_line_ ||
        selection_start_column != last_selection_start_column_ ||
        selection_end_line != last_selection_end_line_ ||
        selection_end_column != last_selection_end_column_));
  if (interest_.selection_change && selection_changed) {
    selection_dirty_ = true;
    selection_deadline_ = deadline;
    selection_present_ = selection_present;
    selection_start_line_ = selection_start_line;
    selection_start_column_ = selection_start_column;
    selection_end_line_ = selection_end_line;
    selection_end_column_ = selection_end_column;
  }

  adopt_baseline();
}

std::optional<std::uint32_t> PluginEditorEventTracker::NextDelayMs(std::uint64_t now_ms) const {
  std::optional<std::uint32_t> best;
  const auto consider = [&](bool dirty, std::uint64_t deadline) {
    if (!dirty) {
      return;
    }
    const std::uint64_t remaining = deadline > now_ms ? deadline - now_ms : 0;
    const auto remaining_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, 0xFFFFFFFFu));
    if (!best.has_value() || remaining_ms < *best) {
      best = remaining_ms;
    }
  };
  consider(change_dirty_, change_deadline_);
  consider(cursor_dirty_, cursor_deadline_);
  consider(selection_dirty_, selection_deadline_);
  return best;
}

PluginEditorEventTracker::DueEvents PluginEditorEventTracker::TakeDue(std::uint64_t now_ms) {
  DueEvents due;
  due.path = path_;
  if (change_dirty_ && change_deadline_ <= now_ms) {
    due.change = true;
    due.change_start_line = change_start_line_;
    due.change_end_line = change_end_line_;
    change_dirty_ = false;
  }
  if (cursor_dirty_ && cursor_deadline_ <= now_ms) {
    due.cursor = true;
    due.cursor_line = cursor_line_;
    due.cursor_column = cursor_column_;
    cursor_dirty_ = false;
  }
  if (selection_dirty_ && selection_deadline_ <= now_ms) {
    due.selection = true;
    due.selection_present = selection_present_;
    due.selection_start_line = selection_start_line_;
    due.selection_start_column = selection_start_column_;
    due.selection_end_line = selection_end_line_;
    due.selection_end_column = selection_end_column_;
    selection_dirty_ = false;
  }
  return due;
}

}  // namespace microide::workspace
