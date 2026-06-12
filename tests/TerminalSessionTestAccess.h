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
    session.lines_ = {microide::terminal::TerminalLine{}};
    session.primary_screen_ = microide::terminal::TerminalSession::ScreenState{};
    session.alternate_screen_ = microide::terminal::TerminalSession::ScreenState{};
    session.working_directory_.clear();
    session.default_launch_label_.clear();
    session.launch_label_.clear();
    session.current_style_ = microide::terminal::TerminalStyle{};
    session.escape_sequence_buffer_.clear();
    session.pending_utf8_sequence_.clear();
    session.wake_event_type_ = 0;
    session.backend_.reset();
    session.child_pid_ = -1;
    session.running_ = false;
    session.stop_requested_ = false;
    session.wake_event_pending_ = false;
    session.escape_mode_ = microide::terminal::TerminalSession::EscapeMode::None;
    session.osc_escape_pending_ = false;
    session.use_alternate_screen_ = false;
    session.mouse_tracking_normal_ = false;
    session.mouse_tracking_drag_ = false;
    session.mouse_tracking_any_ = false;
    session.mouse_sgr_ext_mode_ = false;
    session.application_cursor_keys_mode_ = false;
    session.origin_mode_ = false;
    session.auto_wrap_mode_ = true;
    session.bracketed_paste_mode_ = false;
    session.focus_event_mode_ = false;
    session.cursor_visible_ = true;
    session.synchronized_output_ = false;
    session.sync_suppressed_wakes_ = 0;
    session.kitty_keyboard_flags_ = 0;
    session.kitty_keyboard_stack_.clear();
    session.cursor_shape_ = microide::terminal::TerminalSession::CursorShape::Block;
    session.cursor_blinking_ = true;
    session.tab_stops_.clear();
    session.reported_working_directory_.clear();
    session.rows_ = std::max<std::size_t>(1, rows);
    session.columns_ = std::max<std::size_t>(1, columns);
    session.cursor_row_ = 0;
    session.cursor_column_ = 0;
    session.saved_cursor_row_ = 0;
    session.saved_cursor_column_ = 0;
    session.snapshot_generation_ = 1;
    session.ResetScrollRegionLocked();
#ifdef MICROIDE_TESTING
    session.test_sent_bytes_.clear();
#endif
  }

  static void AppendOutput(microide::terminal::TerminalSession& session, std::string_view data) {
    std::scoped_lock lock(session.mutex_);
    session.AppendOutputLocked(data);
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
};

}  // namespace microide::tests
