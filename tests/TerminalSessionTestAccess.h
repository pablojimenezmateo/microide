#pragma once

#include "terminal/TerminalSession.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

namespace microide::tests {

struct TerminalSessionTestAccess {
  static std::string PredictionState(microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return "pending=" + std::to_string(session.prediction_.pending()) +
           " suppressed=" + std::to_string(session.prediction_.suppressed() ? 1 : 0) +
           " drawing=" + std::to_string(session.prediction_.drawing() ? 1 : 0) +
           " cursor=" + std::to_string(session.cursor_row_) + ":" + std::to_string(session.cursor_column_);
  }

  static void Reset(microide::terminal::TerminalSession& session,
                    std::size_t rows,
                    std::size_t columns) {
    std::scoped_lock lock(session.mutex_);
    session.rows_ = std::max<std::size_t>(1, rows);
    session.columns_ = std::max<std::size_t>(1, columns);
    session.ResetEmulationStateLocked();

    session.lines_ = {microide::terminal::TerminalLine{}};
    session.working_directory_.clear();
    session.default_launch_label_.clear();
    session.launch_label_.clear();
    session.wake_channel_ = 0;
    session.wake_event_pending_ = false;
    session.backend_.reset();
    session.scrollback_trim_total_ = 0;
    session.snapshot_generation_.Bump();
#ifdef MICROIDE_TESTING
    session.test_sent_bytes_.clear();
#endif
  }

  static bool AppendOutputAndDecideWake(microide::terminal::TerminalSession& session, std::string_view data) {
    std::scoped_lock lock(session.mutex_);
    session.AppendOutputLocked(data);
    return session.ConsumeWakeDecisionLocked();
  }

  static void AppendOutput(microide::terminal::TerminalSession& session, std::string_view data) {
    {
      std::scoped_lock lock(session.mutex_);
      session.AppendOutputLocked(data);
    }
    session.FlushPendingReply();
  }

  static std::uint16_t InternLink(microide::terminal::TerminalSession& session,
                                  std::string_view uri) {
    std::scoped_lock lock(session.mutex_);
    return session.InternExtraLocked(uri, {});
  }

  static std::size_t ExtraCount(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.extras_.size();
  }

  static microide::terminal::TerminalColor UnderlineColorOf(
      const microide::terminal::TerminalSession& session, std::uint16_t extra) {
    std::scoped_lock lock(session.mutex_);
    return session.extras_.UnderlineColor(extra);
  }

  static std::size_t LineBufferPoolSize(const microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    return session.line_buffer_pool_.size();
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

  static void MarkOutputArrived(microide::terminal::TerminalSession& session) {
    std::scoped_lock lock(session.mutex_);
    session.wake_event_pending_ = true;
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
    session.mouse_.normal = normal;
    session.mouse_.drag = drag;
    session.mouse_.any = any;
  }

  static void EnterHostMode(microide::terminal::TerminalSession& session,
                            std::shared_ptr<microide::terminal::TerminalHostChannel> channel) {
    std::scoped_lock lock(session.mutex_);
    if (channel != nullptr && session.host_.channel == channel) {
      return;
    }
    session.host_.channel = std::move(channel);
    session.host_.screen_top = 0;
    session.host_.screen_lines = session.lines_.size();
    session.host_.alternate = false;
    session.running_ = true;
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

  static void ClearSentBytes(microide::terminal::TerminalSession& session) {
#ifdef MICROIDE_TESTING
    std::scoped_lock lock(session.mutex_);
    session.test_sent_bytes_.clear();
#else
    (void)session;
#endif
  }

  static bool ReserveWakeChannel(const microide::terminal::TerminalSession& session,
                                 microide::util::WakeChannel& channel) {
    return session.ReserveWakeChannel(channel);
  }

  static void PushWakeEvent(const microide::terminal::TerminalSession& session) {
    session.PushWakeEvent();
  }

  static std::size_t RestoreWithSavedPrimaryCursorRow(microide::terminal::TerminalSession& session,
                                                      std::size_t saved_row) {
    std::scoped_lock lock(session.mutex_);
    session.use_alternate_screen_ = false;
    session.primary_screen_.lines.clear();
    session.primary_screen_.cursor_row = saved_row;
    session.primary_screen_.cursor_column = 0;
    session.RestoreSavedScreenLocked();
    return session.lines_.size();
  }
};

}
