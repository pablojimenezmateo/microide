#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "plugin/PluginHost.h"

namespace microide::workspace {

// Coalesces reactive editor-state changes (content / caret / selection) into
// debounced plugin events (SEAM 1). The shell samples the active editable
// viewport once per input batch; this tracker diffs against its baseline and
// arms a trailing debounce deadline per event kind, so a burst of keystrokes
// produces a single `on_buffer_change` after typing settles.
//
// Zero-cost when nothing subscribes: `SetInterest({})` makes `Sample` a no-op and
// `NextDelayMs` return nullopt, so the shell's idle loop never schedules a wake.
class PluginEditorEventTracker {
 public:
  // What the host resolved to fire, ready for dispatch. Positions are 1-based.
  struct DueEvents {
    std::filesystem::path path;
    bool change = false;
    std::size_t change_start_line = 0;  // inclusive
    std::size_t change_end_line = 0;
    bool cursor = false;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    bool selection = false;
    bool selection_present = false;
    std::size_t selection_start_line = 0;
    std::size_t selection_start_column = 0;
    std::size_t selection_end_line = 0;
    std::size_t selection_end_column = 0;
    bool any() const { return change || cursor || selection; }
  };

  void SetInterest(plugin::PluginHost::EditorEventInterest interest);
  const plugin::PluginHost::EditorEventInterest& interest() const { return interest_; }
  // Drop baselines and any armed events (called on plugin reload).
  void Reset();

  // Sample the active editable buffer. The first sample (or one after the buffer
  // identity changes) re-baselines silently; later samples diff against it and
  // arm trailing deadlines. All positions are 1-based.
  void Sample(const std::filesystem::path& path,
              std::uint64_t content_revision,
              std::size_t cursor_line,
              std::size_t cursor_column,
              bool selection_present,
              std::size_t selection_start_line,
              std::size_t selection_start_column,
              std::size_t selection_end_line,
              std::size_t selection_end_column,
              std::uint64_t now_ms,
              std::uint32_t debounce_ms);

  // Smallest remaining debounce delay across armed kinds, or nullopt when idle.
  std::optional<std::uint32_t> NextDelayMs(std::uint64_t now_ms) const;

  // Collect and clear every kind whose deadline has elapsed.
  DueEvents TakeDue(std::uint64_t now_ms);

 private:
  plugin::PluginHost::EditorEventInterest interest_;

  bool has_baseline_ = false;
  std::filesystem::path path_;
  std::uint64_t last_revision_ = 0;
  std::size_t last_cursor_line_ = 0;
  std::size_t last_cursor_column_ = 0;
  bool last_selection_present_ = false;
  std::size_t last_selection_start_line_ = 0;
  std::size_t last_selection_start_column_ = 0;
  std::size_t last_selection_end_line_ = 0;
  std::size_t last_selection_end_column_ = 0;

  bool change_dirty_ = false;
  std::uint64_t change_deadline_ = 0;
  std::size_t change_start_line_ = 0;
  std::size_t change_end_line_ = 0;

  bool cursor_dirty_ = false;
  std::uint64_t cursor_deadline_ = 0;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;

  bool selection_dirty_ = false;
  std::uint64_t selection_deadline_ = 0;
  bool selection_present_ = false;
  std::size_t selection_start_line_ = 0;
  std::size_t selection_start_column_ = 0;
  std::size_t selection_end_line_ = 0;
  std::size_t selection_end_column_ = 0;
};

}  // namespace microide::workspace
