#pragma once

#include "terminal/TerminalSession.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

namespace microide::tests {

struct TerminalSessionTestAccess {
  static void Reset(microide::terminal::TerminalSession& session,
                    std::size_t rows,
                    std::size_t columns) {
    std::scoped_lock lock(session.mutex_);
    // Emulation state comes from the production reset, not a hand-kept copy of it.
    // This used to restate ~35 fields inline, which meant every new TerminalSession
    // member had to be remembered in two places; it had already fallen behind on
    // pending_clipboard_text_, so a test fixture could start with a stale OSC-52
    // clipboard payload attached. Geometry is seeded first because
    // ResetEmulationStateLocked derives the scroll region from rows_.
    session.rows_ = std::max<std::size_t>(1, rows);
    session.columns_ = std::max<std::size_t>(1, columns);
    session.ResetEmulationStateLocked();

    // What a fresh *fixture* needs on top of the emulation reset. Unlike the
    // production start path this also zeroes scrollback_trim_total_ and drops the
    // wake event type: a test session is a new object, not a restarted one, so
    // nothing can be holding a search token or an event id from before.
    session.lines_ = {microide::terminal::TerminalLine{}};
    session.working_directory_.clear();
    session.default_launch_label_.clear();
    session.launch_label_.clear();
    session.wake_event_type_ = 0;
    session.wake_event_pending_ = false;
    session.backend_.reset();
    session.scrollback_trim_total_ = 0;
    session.snapshot_generation_ = 1;
#ifdef MICROIDE_TESTING
    session.test_sent_bytes_.clear();
#endif
  }

  static void AppendOutput(microide::terminal::TerminalSession& session, std::string_view data) {
    {
      std::scoped_lock lock(session.mutex_);
      session.AppendOutputLocked(data);
    }
    // Mirror the production reader thread: query replies are buffered under the
    // lock and flushed once it is released.
    session.FlushPendingReply();
  }

  static void EmitProcessExitMarker(microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    session.EmitProcessExitMarkerLocked();
  }

  static void SetLaunchLabel(microide::terminal::TerminalSession& session, std::string_view label) {
    std::scoped_lock lock(session.mutex_);
    session.default_launch_label_ = std::string(label);
    session.launch_label_ = session.default_launch_label_;
  }

  static void SetRunning(microide::terminal::TerminalSession& session, bool running) {
    std::scoped_lock lock(session.mutex_);
    session.running_ = running;
  }

  static void SetCursorVisible(microide::terminal::TerminalSession& session, bool visible) {
    std::scoped_lock lock(session.mutex_);
    session.cursor_visible_ = visible;
  }

  static void SetCursorPosition(microide::terminal::TerminalSession& session,
                                std::size_t row,
                                std::size_t column) {
    std::scoped_lock lock(session.mutex_);
    session.cursor_row_ = std::min(row, session.rows_ == 0 ? std::size_t{0} : session.rows_ - 1);
    session.cursor_column_ =
        std::min(column, session.columns_ == 0 ? std::size_t{0} : session.columns_ - 1);
  }

  static void SetChildProcess(microide::terminal::TerminalSession& session, int child_pid) {
    std::scoped_lock lock(session.mutex_);
    session.child_pid_ = child_pid;
    session.running_ = child_pid > 0;
  }

  static void SetMouseTracking(microide::terminal::TerminalSession& session,
                               bool normal,
                               bool drag,
                               bool any) {
    std::scoped_lock lock(session.mutex_);
    session.mouse_tracking_normal_ = normal;
    session.mouse_tracking_drag_ = drag;
    session.mouse_tracking_any_ = any;
  }

  static bool SynchronizedOutput(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.synchronized_output_;
  }

  static unsigned KittyKeyboardFlags(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.kitty_keyboard_flags_;
  }

  static std::size_t TabStopCount(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.tab_stops_.size();
  }

  static std::size_t ScrollRegionBottom(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.scroll_region_bottom_;
  }

  static std::string SentBytes(const microide::terminal::TerminalSession& session) {
#ifdef MICROIDE_TESTING
    std::scoped_lock lock(session.mutex_);
    return session.test_sent_bytes_;
#else
    (void)session;
    return {};
#endif
  }

  static bool ReserveWakeEvent(const microide::terminal::TerminalSession& session,
                               Uint32& event_type) {
    return session.ReserveWakeEvent(event_type);
  }

  // TD-2026-07-17-087: drive the private wake producer directly so a test can force
  // a rejected SDL push (via util::SetSdlEventPusherForTesting) and assert the
  // owed-wake backstop latches.
  static void PushWakeEvent(const microide::terminal::TerminalSession& session) {
    session.PushWakeEvent();
  }

  // TD-2026-07-17-067: inject an oversized saved primary cursor row and run the
  // restore path. Primary restores do not clamp the saved row (only alternate
  // restores do), so this exercises the scrollback cap inside
  // EnsureCursorLineExistsLocked. Returns the resulting line count so the caller
  // can assert the allocation stayed within the scrollback+visible budget.
  static std::size_t RestoreWithSavedPrimaryCursorRow(microide::terminal::TerminalSession& session,
                                                      std::size_t saved_row) {
    std::scoped_lock lock(session.mutex_);
    session.use_alternate_screen_ = false;
    session.primary_screen_.lines.clear();  // force the empty-buffer restore branch
    session.primary_screen_.cursor_row = saved_row;
    session.primary_screen_.cursor_column = 0;
    session.RestoreSavedScreenLocked();
    return session.lines_.size();
  }
};

}  // namespace microide::tests
