#include "terminal/TerminalSession.h"

#include <algorithm>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "terminal/TerminalInternalConstants.h"

namespace microide::terminal {

namespace {

// The rule drawn where host lines could not be had (trimmed on the host while
// detached, or withheld while the credit window was full): one line, with the
// count, never `count` blank lines.
TerminalLine MakeGapLine(std::uint64_t count) {
  TerminalLine line;
  const std::string text = "\xe2\x8b\xaf " + std::to_string(count) +
                           (count == 1 ? " line" : " lines") + " not transferred \xe2\x8b\xaf";
  TerminalStyle style;
  style.set(cell_attr::kDim, true);
  // Glyph by glyph: "⋯" is three bytes and one column.
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t length = static_cast<unsigned char>(text[at]) >= 0xe0 ? 3 : 1;
    TerminalCell cell;
    cell.SetUtf8(std::string_view(text).substr(at, length));
    cell.style = style;
    line.cells.push_back(cell);
    at += length;
  }
  return line;
}

}  // namespace

const HostTerminalSource* HostTerminalsFor(const platform::ProcessLauncher& launcher) {
  // A sideways cast, as project::GitMetadataFor does: a remote launcher opts in by
  // implementing the interface.
  return dynamic_cast<const HostTerminalSource*>(&launcher);
}

bool TerminalSession::is_host_terminal() const {
  std::scoped_lock lock(mutex_);
  return host_channel_ != nullptr;
}

std::shared_ptr<TerminalHostChannel> TerminalSession::HostChannel() const {
  std::scoped_lock lock(mutex_);
  return host_channel_;
}

bool TerminalSession::StartOnHost(const HostTerminalSource& source,
                                  const std::filesystem::path& working_directory,
                                  std::string_view command, std::vector<std::string> shell_argv) {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::size_t scrollback = 0;
  {
    std::scoped_lock lock(mutex_);
    ReseedForStartLocked(working_directory,
                         command.empty() ? std::string("shell") : std::string(command));
    rows = rows_;
    columns = columns_;
    scrollback = max_scrollback_lines_;
    host_screen_top_ = 0;
    host_screen_lines_ = lines_.size();
    host_alternate_ = false;
    host_primary_stash_.clear();
    host_primary_stash_screen_lines_ = 0;
    host_mirrored_ = false;
    next_input_seq_ = 1;
    stop_requested_ = false;
  }
  std::string error;
  std::shared_ptr<TerminalHostChannel> channel = source.OpenTerminal(
      HostTerminalSource::OpenRequest{.working_directory = working_directory,
                                      .command = std::string(command),
                                      .shell = std::move(shell_argv),
                                      .rows = rows,
                                      .columns = columns,
                                      .scrollback_lines = scrollback},
      *this, &error);
  {
    std::scoped_lock lock(mutex_);
    host_channel_ = channel;
    running_ = channel != nullptr;
    if (channel == nullptr) {
      lines_ = {TerminalLine{}};
      cursor_row_ = 0;
      cursor_column_ = 0;
      for (const char c : "[could not open a terminal on the host: " + error + "]") {
        if (c != '\0') {
          TerminalCell cell;
          cell.SetAscii(c);
          lines_.back().cells.push_back(cell);
        }
      }
      host_screen_lines_ = lines_.size();
    }
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
  return channel != nullptr;
}

TerminalPredictionOverlay::View TerminalSession::PredictionViewLocked() {
  return TerminalPredictionOverlay::View{.lines = lines_,
                                         .cursor_row = cursor_row_,
                                         .cursor_column = cursor_column_,
                                         .columns = std::max<std::size_t>(1, columns_),
                                         .cursor_visible = cursor_visible_,
                                         .alternate_screen = use_alternate_screen_};
}

void TerminalSession::SetPredictionMode(TerminalPredictionOverlay::Mode mode) {
  std::scoped_lock lock(mutex_);
  if (mode == TerminalPredictionOverlay::Mode::Never && prediction_.Withdraw(PredictionViewLocked())) {
    AdvanceSnapshotGenerationLocked();
  }
  prediction_.set_mode(mode);
}

std::size_t TerminalSession::PendingPredictions() const {
  std::scoped_lock lock(mutex_);
  return prediction_.pending();
}

bool TerminalSession::SendToHost(TerminalInputEvent event) {
  std::shared_ptr<TerminalHostChannel> channel;
  {
    std::scoped_lock lock(mutex_);
    if (host_channel_ == nullptr) {
      return false;
    }
    channel = host_channel_;
    event.seq = next_input_seq_++;
  }
  // Sent first: the prediction is drawn in this frame either way, and the host
  // should not wait on the overlay.
  channel->Send(event);
  // Asked OUTSIDE the session lock: the channel takes its own lock, and it holds
  // that one while it applies a frame, which takes this one (lock order).
  const std::optional<std::chrono::milliseconds> round_trip = channel->RoundTrip();
  bool predicted = false;
  {
    std::scoped_lock lock(mutex_);
    if (host_channel_ == channel) {
      prediction_.set_round_trip(round_trip);
      predicted = prediction_.Typed(event, PredictionViewLocked());
      if (predicted) {
        AdvanceSnapshotGenerationLocked();
      }
    }
  }
  if (predicted) {
    PushWakeEvent();
  }
  return true;
}

bool TerminalSession::ApplyHostFrame(TerminalHostFrame frame) {
  bool consistent = true;
  {
    std::scoped_lock lock(mutex_);
    if (host_channel_ == nullptr) {
      return true;  // stopped: a late frame has nothing to apply to
    }
    // Predictions out first: Keep and Promote name CONFIRMED lines.
    prediction_.Withdraw(PredictionViewLocked());
    const bool alternate = frame.has(TerminalHostFrame::kAlternateScreen);
    if (frame.has(TerminalHostFrame::kReset)) {
      prediction_.Clear();
      // A cold attach: nothing held is valid, scrollback included.
      lines_.clear();
      host_screen_lines_ = 0;
      host_primary_stash_.clear();
      host_primary_stash_screen_lines_ = 0;
      host_alternate_ = false;
      host_screen_top_ = frame.previous_top;
    }
    if (alternate && !host_alternate_) {
      host_primary_stash_ = std::move(lines_);
      host_primary_stash_screen_lines_ = host_screen_lines_;
      lines_.clear();
      host_screen_lines_ = 0;
    } else if (!alternate && host_alternate_) {
      lines_ = std::move(host_primary_stash_);
      host_primary_stash_.clear();
      host_screen_lines_ = host_primary_stash_screen_lines_;
    }
    host_alternate_ = alternate;

    // Detach the old screen from the tail; what stays is scrollback.
    std::vector<TerminalLine> old_screen;
    old_screen.reserve(host_screen_lines_);
    const std::size_t screen_from = lines_.size() - std::min(host_screen_lines_, lines_.size());
    for (std::size_t i = screen_from; i < lines_.size(); ++i) {
      old_screen.push_back(std::move(lines_[i]));
    }
    lines_.resize(screen_from);
    const std::uint64_t old_top = alternate ? 0 : host_screen_top_;
    const auto take_old = [&](std::uint64_t absolute, TerminalLine& out) {
      if (absolute < old_top || absolute - old_top >= old_screen.size()) {
        consistent = false;
        out = TerminalLine{};
        return;
      }
      out = std::move(old_screen[static_cast<std::size_t>(absolute - old_top)]);
    };

    if (!alternate) {
      if (frame.previous_top != host_screen_top_) {
        consistent = false;
      }
      if (frame.screen_top < frame.previous_top) {
        // The screen grew upward (a taller window): its new top rows were our
        // scrollback, and they come again as screen rows.
        const std::uint64_t back = frame.previous_top - frame.screen_top;
        lines_.resize(lines_.size() - static_cast<std::size_t>(std::min<std::uint64_t>(
                                          back, lines_.size())));
      }
      std::uint64_t absolute = frame.previous_top;
      std::size_t next_line = 0;
      for (const TerminalHostRun& run : frame.scrollback) {
        switch (run.kind) {
          case TerminalHostRun::Kind::Promote:
            for (std::uint64_t i = 0; i < run.count; ++i) {
              take_old(absolute + i, lines_.emplace_back());
            }
            break;
          case TerminalHostRun::Kind::Lines:
            for (std::uint64_t i = 0; i < run.count; ++i) {
              lines_.push_back(std::move(frame.scrollback_lines[next_line++]));
            }
            break;
          case TerminalHostRun::Kind::Gap:
            lines_.push_back(MakeGapLine(run.count));
            break;
        }
        absolute += run.count;
      }
    }

    const std::uint64_t top = alternate ? 0 : frame.screen_top;
    std::size_t next_row = 0;
    for (std::size_t row = 0; row < frame.screen_keep.size(); ++row) {
      if (frame.screen_keep[row]) {
        take_old(top + row, lines_.emplace_back());
      } else {
        lines_.push_back(std::move(frame.screen_lines[next_row++]));
      }
    }
    if (frame.screen_keep.empty()) {
      lines_.emplace_back();  // the buffer always holds a line
    }
    host_screen_lines_ = std::max<std::size_t>(1, frame.screen_keep.size());
    if (!alternate) {
      host_screen_top_ = frame.screen_top;
    }
    host_mirrored_ = true;

    rows_ = frame.rows;
    columns_ = frame.columns;
    // The client's own scrollback cap; the host's is the host's business.
    if (!alternate) {
      const std::size_t keep = max_scrollback_lines_ + host_screen_lines_;
      if (lines_.size() > keep + keep / 4) {
        const std::size_t trim = lines_.size() - keep;
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim));
        scrollback_trim_total_ += trim;
      }
    }
    cursor_row_ = lines_.size() - host_screen_lines_ + frame.cursor_row;
    cursor_column_ = frame.cursor_column;
    use_alternate_screen_ = alternate;
    cursor_visible_ = frame.has(TerminalHostFrame::kCursorVisible);
    cursor_blinking_ = frame.has(TerminalHostFrame::kCursorBlinking);
    cursor_shape_ = frame.cursor_shape;
    mouse_tracking_normal_ = frame.has(TerminalHostFrame::kMouseNormal);
    mouse_tracking_drag_ = frame.has(TerminalHostFrame::kMouseDrag);
    mouse_tracking_any_ = frame.has(TerminalHostFrame::kMouseAny);
    focus_event_mode_ = frame.has(TerminalHostFrame::kFocusEvents);
    running_ = frame.has(TerminalHostFrame::kRunning);
    if (frame.has(TerminalHostFrame::kBell)) {
      pending_bell_ = true;
    }
    if (frame.title) {
      launch_label_ = frame.title->empty() ? default_launch_label_ : std::move(*frame.title);
    }
    if (frame.working_directory) {
      reported_working_directory_ = std::move(*frame.working_directory);
    }
    if (frame.clipboard) {
      pending_clipboard_text_ = std::move(*frame.clipboard);
    }
    prediction_.Judge(frame.echo_ack, PredictionViewLocked());
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
  return consistent;
}

void TerminalSession::HostConnectionLost(std::string_view reason) {
  {
    std::scoped_lock lock(mutex_);
    if (host_channel_ == nullptr || !running_) {
      return;
    }
    running_ = false;
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    const std::string text = "[connection to the host lost: " + std::string(reason) + "]";
    TerminalLine line;
    for (const char c : text) {
      TerminalCell cell;
      cell.SetAscii(c);
      line.cells.push_back(cell);
    }
    // As scrollback below the screen, so the screen's mirror is untouched and a
    // reattach can still resume against it.
    lines_.insert(lines_.end() - static_cast<std::ptrdiff_t>(std::min(host_screen_lines_, lines_.size())),
                  std::move(line));
    ++cursor_row_;
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

std::optional<TerminalSession::HostResumePoint> TerminalSession::host_resume_point() const {
  std::scoped_lock lock(mutex_);
  if (host_channel_ == nullptr || !host_mirrored_) {
    return std::nullopt;
  }
  return HostResumePoint{.screen_top = host_screen_top_, .alternate = host_alternate_};
}

void TerminalSession::ReattachHost(std::shared_ptr<TerminalHostChannel> channel) {
  {
    std::scoped_lock lock(mutex_);
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    host_channel_ = std::move(channel);
    // The new client numbers its input from 1 again; so does the host.
    next_input_seq_ = 1;
    running_ = host_channel_ != nullptr;
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

void TerminalSession::SetOutputObserver(std::function<void()> observer) {
  std::scoped_lock lock(mutex_);
  output_observer_ = std::move(observer);
}

void TerminalSession::CaptureForHost(std::uint64_t from, std::size_t max_lines_before_screen,
                                     HostCapture& out) {
  std::scoped_lock lock(mutex_);
  TerminalHostFrame& header = out.header;
  header.rows = static_cast<std::uint32_t>(std::max<std::size_t>(1, rows_));
  header.columns = static_cast<std::uint32_t>(std::max<std::size_t>(1, columns_));
  header.flags = 0;
  const auto flag = [&](std::uint16_t bit, bool on) {
    if (on) {
      header.flags |= bit;
    }
  };
  flag(TerminalHostFrame::kAlternateScreen, use_alternate_screen_);
  flag(TerminalHostFrame::kCursorVisible, cursor_visible_);
  flag(TerminalHostFrame::kCursorBlinking, cursor_blinking_);
  flag(TerminalHostFrame::kMouseNormal, mouse_tracking_normal_);
  flag(TerminalHostFrame::kMouseDrag, mouse_tracking_drag_);
  flag(TerminalHostFrame::kMouseAny, mouse_tracking_any_);
  flag(TerminalHostFrame::kFocusEvents, focus_event_mode_);
  flag(TerminalHostFrame::kRunning, running_);
  flag(TerminalHostFrame::kBell, pending_host_bell_);
  pending_host_bell_ = false;
  header.cursor_shape = cursor_shape_;

  const std::size_t screen_row = PrimaryScreenTopLocked();
  const std::uint64_t screen_top = use_alternate_screen_ ? 0 : scrollback_trim_total_ + screen_row;
  header.screen_top = screen_top;
  header.cursor_row = static_cast<std::uint32_t>(std::min<std::size_t>(
      cursor_row_ >= screen_row ? cursor_row_ - screen_row : 0, header.rows - 1));
  header.cursor_column =
      static_cast<std::uint32_t>(std::min<std::size_t>(cursor_column_, header.columns - 1));
  header.title = launch_label_;
  header.working_directory = reported_working_directory_.string();
  header.clipboard = std::move(pending_clipboard_text_);
  pending_clipboard_text_.reset();

  // Lines from max(from, what is held, the cap) through the end.
  std::uint64_t first = use_alternate_screen_ ? 0 : std::max(from, scrollback_trim_total_);
  if (!use_alternate_screen_ && screen_top - std::min(first, screen_top) > max_lines_before_screen) {
    first = screen_top - max_lines_before_screen;
  }
  first = std::min(first, screen_top);
  const std::size_t begin = use_alternate_screen_
                                ? 0
                                : static_cast<std::size_t>(first - scrollback_trim_total_);
  out.first_line = first;
  out.lines.resize(lines_.size() - begin);
  for (std::size_t i = begin; i < lines_.size(); ++i) {
    TerminalLine& dest = out.lines[i - begin];
    dest.cells.assign(lines_[i].cells.begin(), lines_[i].cells.end());
    dest.wrapped_from_previous = lines_[i].wrapped_from_previous;
  }
  out.generation = snapshot_generation_;
}

}  // namespace microide::terminal
