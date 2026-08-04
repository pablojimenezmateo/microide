#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalOscClipboard.h"
#include "terminal/TerminalSessionInputEncoding.h"
#include "platform/ShellProcess.h"
#include "platform/TerminalBackend.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/SdlWake.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::terminal {

namespace {

// A shrink can strand the cursor above the visible window. The primary buffer's
// viewport is the LAST `rows` lines of the deque and the cursor is an absolute
// index into it, so shrinking `rows` moves the viewport down past a cursor sitting
// higher up — and nothing brings it back, because the shell keeps writing to rows
// that are now permanently off-screen. The reachable case is a `clear`
// (ESC[H ESC[2J ESC[3J) issued before the panel's real geometry is known: it leaves
// exactly one pre-layout screen of lines with the cursor on the first, and the
// resize that follows parks the viewport on the blank tail.
//
// xterm and VTE drop the unused blank remainder of the old screen on a shrink, so
// do that — and only that. Lines below the cursor that still hold text are kept
// (they are real output), and nothing is dropped unless the cursor is genuinely
// stranded, so an ordinary shrink over scrollback is untouched.
void DropBlankTailStrandingCursor(std::deque<TerminalLine>& lines,
                                  std::size_t rows,
                                  std::size_t cursor_row) {
  if (rows == 0) {
    return;
  }
  const auto line_is_blank = [](const TerminalLine& line) {
    for (const TerminalCell& cell : line.cells) {
      const std::string_view text = cell.DisplayText();
      if (!text.empty() && text != " ") {
        return false;
      }
    }
    return true;
  };
  // `cursor_row + rows < lines.size()` is exactly "the cursor is above the visible
  // top" (which is lines.size() - rows).
  while (lines.size() > rows && cursor_row + rows < lines.size() &&
         line_is_blank(lines.back())) {
    lines.pop_back();
  }
}

// Process-wide flag backing UsePlaceholderTerminalsForTesting(). Set once by the
// test harness before any terminal is created and never mutated concurrently, so
// a plain bool is sufficient.
bool g_use_placeholder_terminals_for_testing = false;

using platform::DefaultShellPath;
using platform::RequestTerminalChildShutdown;
using platform::ShellProgramName;

}  // namespace

void SetUsePlaceholderTerminalsForTesting(bool enabled) {
  g_use_placeholder_terminals_for_testing = enabled;
}

bool UsePlaceholderTerminalsForTesting() {
  return g_use_placeholder_terminals_for_testing;
}

TerminalSession::~TerminalSession() {
  Stop();
}

void TerminalSession::SetWakeEventType(Uint32 event_type) {
  std::scoped_lock lock(mutex_);
  wake_event_type_ = event_type;
  if (event_type == 0) {
    wake_event_pending_ = false;
  }
}

bool TerminalSession::Start(const std::filesystem::path& working_directory, std::string_view command,
                            std::string_view shell) {
  Stop();
  const std::string shell_str(shell);
  {
    std::scoped_lock lock(mutex_);
    ReseedForStartLocked(working_directory,
                         command.empty()
                             ? ShellProgramName(shell_str.empty() ? DefaultShellPath() : shell_str)
                             : std::string(command));
  }

  std::shared_ptr<platform::TerminalBackend> backend = platform::CreateTerminalBackend();
  platform::TerminalBackend* backend_ptr = backend.get();
  // Publish the backend BEFORE starting it. The backend's on_output callback can fire
  // synchronously during Start() (a shell that immediately answers a DSR/DA query), and
  // FlushPendingReply()->SendBytes() needs to find backend_ to write the reply — if we
  // assigned it only after Start(), that early reply would be silently dropped.
  {
    std::scoped_lock lock(mutex_);
    backend_ = backend;
  }
  auto callbacks = platform::TerminalBackendCallbacks{
      .on_output =
          [this](std::string_view output) {
            bool wake = true;
            {
              std::scoped_lock lock(mutex_);
              AppendOutputLocked(output);
              wake = ConsumeWakeDecisionLocked();
            }
            // Flush query replies (DSR/DA/etc.) generated during parsing after
            // releasing mutex_, so the blocking PTY write() never runs under the
            // lock on this reader thread.
            FlushPendingReply();
            if (wake) {
              PushWakeEvent();
            }
          },
      .on_exit =
          [this]() {
            {
              std::scoped_lock lock(mutex_);
              const bool emit_exit_marker = !stop_requested_;
              running_ = false;
              child_pid_ = -1;
              if (emit_exit_marker) {
                EmitProcessExitMarkerLocked();
              }
            }
            PushWakeEvent();
          },
  };
  const auto result = backend_ptr->Start(platform::TerminalStartRequest{
                                             .working_directory = working_directory,
                                             .command = std::string(command),
                                             .shell = shell_str,
                                             .rows = rows_,
                                             .columns = columns_,
                                         },
                                         std::move(callbacks));

  {
    std::scoped_lock lock(mutex_);
    default_launch_label_ = result.launch_label.empty() ? default_launch_label_ : result.launch_label;
    launch_label_ = default_launch_label_;
    child_pid_ = result.child_process_id;
    running_ = result.running;
    if (!result.initial_output.empty()) {
      AppendOutputLocked(result.initial_output);
    }
  }
  FlushPendingReply();
  PushWakeEvent();
  return result.started;
}

bool TerminalSession::StartPlaceholderForTesting(const std::filesystem::path& working_directory,
                                                 std::string_view command) {
  Stop();
  {
    std::scoped_lock lock(mutex_);
    ReseedForStartLocked(working_directory, command.empty()
                                                ? ShellProgramName(DefaultShellPath())
                                                : std::string(command));
    // The two differences from the real Start(): there is no backend to publish,
    // and the captured-output buffer that stands in for one starts empty.
    backend_.reset();
    test_sent_bytes_.clear();
  }
  PushWakeEvent();
  return true;
}

void TerminalSession::Stop() {
  std::shared_ptr<platform::TerminalBackend> backend;
  int child_pid = -1;
  {
    std::scoped_lock lock(mutex_);
    stop_requested_ = true;
    backend = std::move(backend_);
    child_pid = child_pid_;
  }
  if (backend) {
    backend->Stop();
#if defined(__unix__) || defined(__APPLE__)
  } else if (child_pid > 0) {
    RequestTerminalChildShutdown(child_pid);
#endif
  }

  std::scoped_lock lock(mutex_);
  default_launch_label_.clear();
  launch_label_.clear();
  ResetEmulationStateLocked();
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::SetMaxScrollbackLines(std::size_t max_lines) {
  const std::size_t clamped = std::clamp<std::size_t>(max_lines, 200, 100000);
  std::scoped_lock lock(mutex_);
  if (max_scrollback_lines_ == clamped) {
    return;
  }
  max_scrollback_lines_ = clamped;
  TrimScrollbackLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::Resize(std::size_t rows, std::size_t columns) {
  util::PerformanceTrace::Scope trace_scope("TerminalSession::Resize");
  const std::size_t clamped_rows = std::max<std::size_t>(1, rows);
  const std::size_t clamped_columns = std::max<std::size_t>(1, columns);

  {
    std::scoped_lock lock(mutex_);
    const std::size_t old_rows = rows_;
    // A scroll region that spanned the full pre-resize screen must follow the new
    // height. ClampScrollRegionLocked only *shrinks* the bottom margin, so after a
    // grow a default full-window region would otherwise stay frozen at the old
    // height and confine all scrolling to the top of the enlarged screen (xterm/VTE
    // reset margins to the full window on resize).
    const bool region_spanned_full_screen =
        scroll_region_top_ == 0 && old_rows > 0 && scroll_region_bottom_ == old_rows - 1;
    rows_ = clamped_rows;
    columns_ = clamped_columns;
    if (region_spanned_full_screen && rows_ > 0) {
      scroll_region_top_ = 0;
      scroll_region_bottom_ = rows_ - 1;
    }
    // Mirror the re-expansion onto the SAVED primary/alternate regions too. The
    // active screen's live fields above are authoritative, but RestoreSavedScreen
    // loads the inactive screen's saved region verbatim — so a full-window region
    // saved before a grow-resize would otherwise restore frozen at the old height
    // (ClampScrollRegionLocked only shrinks, never grows).
    if (old_rows > 0 && rows_ > 0) {
      if (primary_screen_.scroll_region_top == 0 &&
          primary_screen_.scroll_region_bottom == old_rows - 1) {
        primary_screen_.scroll_region_bottom = rows_ - 1;
      }
      if (alternate_screen_.scroll_region_top == 0 &&
          alternate_screen_.scroll_region_bottom == old_rows - 1) {
        alternate_screen_.scroll_region_bottom = rows_ - 1;
      }
    }
    if (use_alternate_screen_ && rows_ > 0) {
      cursor_row_ = std::min(cursor_row_, rows_ - 1);
      saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
    }
    if (columns_ > 0) {
      cursor_column_ = std::min(cursor_column_, columns_ - 1);
      saved_cursor_column_ = std::min(saved_cursor_column_, columns_ - 1);
    }
    primary_screen_.cursor_column = columns_ > 0 ? std::min(primary_screen_.cursor_column, columns_ - 1)
                                                 : primary_screen_.cursor_column;
    primary_screen_.saved_cursor_column =
        columns_ > 0 ? std::min(primary_screen_.saved_cursor_column, columns_ - 1)
                     : primary_screen_.saved_cursor_column;
    if (rows_ > 0) {
      alternate_screen_.cursor_row = std::min(alternate_screen_.cursor_row, rows_ - 1);
      alternate_screen_.saved_cursor_row = std::min(alternate_screen_.saved_cursor_row, rows_ - 1);
    }
    alternate_screen_.cursor_column =
        columns_ > 0 ? std::min(alternate_screen_.cursor_column, columns_ - 1)
                     : alternate_screen_.cursor_column;
    alternate_screen_.saved_cursor_column =
        columns_ > 0 ? std::min(alternate_screen_.saved_cursor_column, columns_ - 1)
                     : alternate_screen_.saved_cursor_column;
    ClampScrollRegionLocked();
    ResizeTabStopsLocked();
    if (use_alternate_screen_) {
      lines_.resize(std::max<std::size_t>(1, rows_));
    }
    EnsureCursorLineExistsLocked();
    if (!use_alternate_screen_) {
      DropBlankTailStrandingCursor(lines_, rows_, cursor_row_);
    }
    TrimScrollbackLocked();
    AdvanceSnapshotGenerationLocked();
  }

  std::shared_ptr<platform::TerminalBackend> backend;
  {
    std::scoped_lock lock(mutex_);
    backend = backend_;  // copy keeps it alive across the unlocked Resize()
  }
  if (backend != nullptr) {
    backend->Resize(clamped_rows, clamped_columns);
  }
}

void TerminalSession::SendBytes(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }
  if (UsePlaceholderTerminalsForTesting()) {
    std::scoped_lock lock(mutex_);
    if (!backend_ || !backend_->running()) {
      test_sent_bytes_.append(bytes);
      return;
    }
  }
  std::shared_ptr<platform::TerminalBackend> backend;
  {
    std::scoped_lock lock(mutex_);
    backend = backend_;  // copy keeps it alive across the unlocked Write()
  }
  if (!backend) {
    if (UsePlaceholderTerminalsForTesting()) {
      std::scoped_lock lock(mutex_);
      test_sent_bytes_.append(bytes);
    }
    return;
  }
  backend->Write(bytes);
}

bool TerminalSession::running() const {
  std::scoped_lock lock(mutex_);
  return running_;
}

std::size_t TerminalSession::LineCount() const {
  std::scoped_lock lock(mutex_);
  return lines_.size();
}

std::uint64_t TerminalSession::ScrollbackTrimTotal() const {
  std::scoped_lock lock(mutex_);
  return scrollback_trim_total_;
}

std::vector<TerminalLine> TerminalSession::SnapshotLines() const {
  std::scoped_lock lock(mutex_);
  return std::vector<TerminalLine>(lines_.begin(), lines_.end());
}

std::vector<TerminalLine> TerminalSession::SnapshotLineRange(std::size_t start_row,
                                                             std::size_t max_lines) const {
  std::scoped_lock lock(mutex_);
  if (start_row >= lines_.size() || max_lines == 0) {
    return {};
  }

  const std::size_t end_row = std::min(lines_.size(), start_row + max_lines);
  return {
      lines_.begin() + static_cast<std::ptrdiff_t>(start_row),
      lines_.begin() + static_cast<std::ptrdiff_t>(end_row),
  };
}

TerminalSession::SearchScan TerminalSession::FindMatches(const TerminalSearchQuery& query,
                                                         std::size_t start_row,
                                                         const std::uint64_t expected_trim_total,
                                                         const std::size_t max_matches,
                                                         TerminalSearchScratch& scratch,
                                                         std::vector<TerminalSearchMatch>& out) const {
  std::scoped_lock lock(mutex_);
  SearchScan scan;
  scan.line_count = lines_.size();
  scan.trim_total = scrollback_trim_total_;
  scan.stable_row_end = PrimaryScreenTopLocked();
  if (scan.trim_total != expected_trim_total) {
    out.clear();
    start_row = 0;
    scan.full_rescan = true;
  }
  if (query.empty() || max_matches == 0) {
    return scan;
  }
  for (std::size_t row = start_row; row < lines_.size(); ++row) {
    if (!FindTerminalLineMatches(lines_[row], query, row, max_matches, scratch, out)) {
      scan.truncated = true;
      break;
    }
  }
  return scan;
}

const std::vector<TerminalLine>& TerminalSession::SnapshotLineRangeCached(
    std::size_t start_row,
    std::size_t max_lines) const {
  thread_local std::vector<TerminalLine> snapshot;
  snapshot = SnapshotLineRange(start_row, max_lines);
  return snapshot;
}

bool TerminalSession::SnapshotLineRangeIfChanged(std::size_t start_row,
                                                 std::size_t max_lines,
                                                 std::uint64_t previous_generation,
                                                 TerminalLineRangeSnapshot* snapshot) const {
  util::AddPerformanceCounter(util::PerfCounterId::TerminalSnapshotLineRangeIfChangedCalls);
  if (snapshot == nullptr) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  if (snapshot_generation_ == previous_generation && snapshot->generation == snapshot_generation_) {
    return false;
  }

  snapshot->generation = snapshot_generation_;
  snapshot->lines.clear();
  if (start_row >= lines_.size() || max_lines == 0) {
    return true;
  }

  const std::size_t end_row = std::min(lines_.size(), start_row + max_lines);
  util::AddPerformanceCounter(util::PerfCounterId::TerminalSnapshotLineRangeIfChangedCopiedLines,
                              end_row - start_row);
  std::uint64_t copied_cells = 0;
  for (auto it = lines_.begin() + static_cast<std::ptrdiff_t>(start_row);
       it != lines_.begin() + static_cast<std::ptrdiff_t>(end_row); ++it) {
    copied_cells += it->cells.size();
  }
  util::AddPerformanceCounter(util::PerfCounterId::TerminalSnapshotLineRangeIfChangedCopiedCells,
                              copied_cells);
  snapshot->lines.assign(lines_.begin() + static_cast<std::ptrdiff_t>(start_row),
                         lines_.begin() + static_cast<std::ptrdiff_t>(end_row));
  return true;
}

std::string TerminalSession::LaunchLabel() const {
  std::scoped_lock lock(mutex_);
  return launch_label_;
}

std::size_t TerminalSession::rows() const {
  std::scoped_lock lock(mutex_);
  return rows_;
}

std::size_t TerminalSession::columns() const {
  std::scoped_lock lock(mutex_);
  return columns_;
}

std::size_t TerminalSession::cursor_row() const {
  std::scoped_lock lock(mutex_);
  return cursor_row_;
}

std::size_t TerminalSession::cursor_column() const {
  std::scoped_lock lock(mutex_);
  return cursor_column_;
}

bool TerminalSession::cursor_visible() const {
  std::scoped_lock lock(mutex_);
  return cursor_visible_;
}

TerminalCursorSnapshot TerminalSession::CursorSnapshot() const {
  std::scoped_lock lock(mutex_);
  return TerminalCursorSnapshot{
      .row = cursor_row_,
      .column = cursor_column_,
      .visible = cursor_visible_,
  };
}

bool TerminalSession::using_alternate_screen() const {
  std::scoped_lock lock(mutex_);
  return use_alternate_screen_;
}

TerminalSession::CursorShape TerminalSession::cursor_shape() const {
  std::scoped_lock lock(mutex_);
  return cursor_shape_;
}

bool TerminalSession::cursor_blinking() const {
  std::scoped_lock lock(mutex_);
  return cursor_blinking_;
}

bool TerminalSession::synchronized_output_active() const {
  std::scoped_lock lock(mutex_);
  return synchronized_output_;
}

std::filesystem::path TerminalSession::reported_working_directory() const {
  std::scoped_lock lock(mutex_);
  return reported_working_directory_;
}

bool TerminalSession::ConsumeWakeEvent() {
  std::scoped_lock lock(mutex_);
  const bool pending = wake_event_pending_;
  wake_event_pending_ = false;
  return pending;
}

bool TerminalSession::WantsMouseCapture() const {
  std::scoped_lock lock(mutex_);
  return CurrentTerminalMouseTrackingMode(mouse_tracking_any_, mouse_tracking_drag_,
                                          mouse_tracking_normal_) != TerminalMouseTrackingMode::Disabled;
}

bool TerminalSession::WantsMouseMotionCapture(bool buttons_down) const {
  std::scoped_lock lock(mutex_);
  switch (CurrentTerminalMouseTrackingMode(mouse_tracking_any_, mouse_tracking_drag_,
                                           mouse_tracking_normal_)) {
    case TerminalMouseTrackingMode::Any:
      return true;
    case TerminalMouseTrackingMode::Drag:
      return buttons_down;
    case TerminalMouseTrackingMode::Normal:
    case TerminalMouseTrackingMode::Disabled:
    default:
      return false;
  }
}

bool TerminalSession::WantsFocusEvents() const {
  std::scoped_lock lock(mutex_);
  return focus_event_mode_;
}

std::optional<std::string> TerminalSession::ConsumePendingClipboardText() {
  std::scoped_lock lock(mutex_);
  std::optional<std::string> pending = std::move(pending_clipboard_text_);
  pending_clipboard_text_.reset();
  return pending;
}

bool TerminalSession::ConsumeOversizedOsc52Dropped() {
  std::scoped_lock lock(mutex_);
  const bool dropped = oversized_osc52_dropped_;
  oversized_osc52_dropped_ = false;
  return dropped;
}

void TerminalSession::SendFocusEvent(bool focused) {
  {
    std::scoped_lock lock(mutex_);
    if (!focus_event_mode_) {
      return;
    }
    SendBytesLocked(focused ? "\x1b[I" : "\x1b[O");
  }
  FlushPendingReply();
}

void TerminalSession::AdvanceSnapshotGenerationLocked() {
  if (++snapshot_generation_ == 0) {
    snapshot_generation_ = 1;
  }
}

bool TerminalSession::ConsumeWakeDecisionLocked() {
  // Under synchronized output (DEC 2026) the application brackets a full frame
  // between `?2026h` and `?2026l`; repainting mid-frame both tears and burns
  // CPU. Suppress the redraw wake while the frame is open, with a safety cap so
  // an application that never closes the frame cannot freeze the display.
  constexpr int kMaxSuppressedWakes = 8;
  if (synchronized_output_ && sync_suppressed_wakes_ < kMaxSuppressedWakes) {
    ++sync_suppressed_wakes_;
    return false;
  }
  sync_suppressed_wakes_ = 0;
  return true;
}

bool TerminalSession::ReserveWakeEvent(Uint32& event_type) const {
  std::scoped_lock lock(mutex_);
  if (wake_event_type_ == 0 || wake_event_pending_) {
    return false;
  }

  wake_event_pending_ = true;
  event_type = wake_event_type_;
  return true;
}

void TerminalSession::PushWakeEvent() const {
  Uint32 event_type = 0;
  if (!ReserveWakeEvent(event_type)) {
    return;
  }

  // TD-2026-07-17-087: route through util::PushSdlWake so a rejected push latches
  // the process-wide owed-wake bit (consumed by the idle-wait poll). Otherwise a
  // dropped final PTY-output or process-exit-marker wake leaves parsed terminal
  // state undrawn until unrelated input wakes the loop. Clear the local coalescing
  // flag on failure so a later producer retries.
  if (!util::PushSdlWake(event_type)) {
    std::scoped_lock lock(mutex_);
    wake_event_pending_ = false;
  }
}

}  // namespace microide::terminal
