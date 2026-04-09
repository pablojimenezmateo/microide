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
    session.launch_label_.clear();
    session.current_style_ = microide::terminal::TerminalStyle{};
    session.escape_sequence_buffer_.clear();
    session.wake_event_type_ = 0;
    session.master_fd_ = -1;
    session.child_pid_ = -1;
    session.running_ = false;
    session.stop_requested_ = false;
    session.escape_mode_ = microide::terminal::TerminalSession::EscapeMode::None;
    session.osc_escape_pending_ = false;
    session.use_alternate_screen_ = false;
    session.mouse_tracking_normal_ = false;
    session.mouse_tracking_drag_ = false;
    session.mouse_tracking_any_ = false;
    session.mouse_sgr_ext_mode_ = false;
    session.bracketed_paste_mode_ = false;
    session.cursor_visible_ = true;
    session.rows_ = std::max<std::size_t>(1, rows);
    session.columns_ = std::max<std::size_t>(1, columns);
    session.cursor_row_ = 0;
    session.cursor_column_ = 0;
    session.saved_cursor_row_ = 0;
    session.saved_cursor_column_ = 0;
    session.ResetScrollRegionLocked();
#ifdef MICROIDE_TESTING
    session.test_sent_bytes_.clear();
#endif
  }

  static void AppendOutput(microide::terminal::TerminalSession& session, std::string_view data) {
    std::scoped_lock lock(session.mutex_);
    session.AppendOutputLocked(data);
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
};

}  // namespace microide::tests
