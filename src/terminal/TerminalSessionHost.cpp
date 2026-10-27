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
std::string GapText(std::uint64_t count) {
  return "\xe2\x8b\xaf " + std::to_string(count) + (count == 1 ? " line" : " lines") +
         " not transferred \xe2\x8b\xaf";
}

std::size_t Utf8Length(unsigned char lead) {
  return lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
}

// The lines the session draws itself (a pending start, a lost link, a gap rule,
// the reattach rule), always through this one builder.
TerminalLine MakeStatusLine(std::string_view text, bool dim) {
  TerminalLine line;
  TerminalStyle style;
  style.set(cell_attr::kDim, dim);
  // Glyph by glyph: "⋯" is three bytes and one column. A truncated sequence at the
  // end (a host's error text cut short) is one cell of what is left.
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t length =
        std::min(Utf8Length(static_cast<unsigned char>(text[at])), text.size() - at);
    TerminalCell cell;
    cell.SetUtf8(text.substr(at, length));
    cell.style = style;
    line.cells.push_back(cell);
    at += length;
  }
  return line;
}

// Insert `line` into a host mirror's `lines` as scrollback just above the screen,
// so the screen's mirror is untouched and a reattach can still resume against
// it. It stands for no host line. The cursor, on the screen, moves down one.
void InsertHostRule(std::deque<TerminalLine>& lines, TerminalHostMode& host,
                    std::uint64_t trim_total, std::size_t& cursor_row, TerminalLine line) {
  const std::size_t at = lines.size() - std::min(host.screen_lines, lines.size());
  host.history.AddGap(trim_total + at, 0);
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
  ++cursor_row;
}

}  // namespace

const HostTerminalSource* HostTerminalsFor(const platform::ProcessLauncher& launcher) {
  // A sideways cast, as project::GitMetadataFor does: a remote launcher opts in by
  // implementing the interface.
  return dynamic_cast<const HostTerminalSource*>(&launcher);
}

bool TerminalSession::is_host_terminal() const {
  std::scoped_lock lock(mutex_);
  return host_.channel != nullptr;
}

std::shared_ptr<TerminalHostChannel> TerminalSession::HostChannel() const {
  std::scoped_lock lock(mutex_);
  return host_.channel;
}

bool TerminalSession::StartOnHost(const HostTerminalSource& source,
                                  const std::filesystem::path& working_directory,
                                  std::string_view command, std::vector<std::string> shell_argv,
                                  HostIdentity attach) {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::size_t scrollback = 0;
  // Asked outside the session lock: the source takes its connection's.
  const bool ready = source.Ready();
  {
    std::scoped_lock lock(mutex_);
    ReseedForStartLocked(working_directory,
                         command.empty() ? std::string("shell") : std::string(command));
    rows = rows_;
    columns = columns_;
    scrollback = max_scrollback_lines_;
    host_.screen_top = 0;
    host_.screen_lines = lines_.size();
    host_.alternate = false;
    host_.primary_stash.reset();
    host_.primary_stash_screen_lines = 0;
    host_.mirrored = false;
    host_.link_down = false;
    host_.next_input_seq = 1;
    stop_requested_ = false;
    if (!ready) {
      // Not connected yet: wait, with the request kept, until StartPendingOnHost.
      // The status line is the caller's to say (SetPendingStatus).
      lines_ = {MakeStatusLine("[waiting for the host connection\xe2\x80\xa6]", /*dim=*/false)};
      host_.screen_lines = lines_.size();
      cursor_visible_ = false;
      running_ = false;
      host_.pending = TerminalHostMode::PendingStart{.working_directory = working_directory,
                                       .command = std::string(command),
                                       .shell_argv = std::move(shell_argv),
                                       .attach = std::move(attach)};
      AdvanceSnapshotGenerationLocked();
    }
  }
  if (!ready) {
    PushWakeEvent();
    return true;
  }
  std::string error;
  std::shared_ptr<TerminalHostChannel> channel = source.OpenTerminal(
      HostTerminalSource::OpenRequest{.working_directory = working_directory,
                                      .command = std::string(command),
                                      .shell = std::move(shell_argv),
                                      .rows = rows,
                                      .columns = columns,
                                      .scrollback_lines = scrollback,
                                      .attach = std::move(attach)},
      *this, &error);
  {
    std::scoped_lock lock(mutex_);
    host_.channel = channel;
    running_ = channel != nullptr;
    if (channel == nullptr) {
      lines_ = {MakeStatusLine("[could not open a terminal on the host: " + error + "]", /*dim=*/false)};
      cursor_row_ = 0;
      cursor_column_ = 0;
      host_.screen_lines = lines_.size();
    }
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
  return channel != nullptr;
}

void TerminalSession::StopHost(const std::shared_ptr<TerminalHostChannel>& channel) {
  // Outside the lock: Close waits out a frame being applied, which takes it.
  if (channel != nullptr) {
    channel->Close();
  }
  std::scoped_lock lock(mutex_);
  if (channel != nullptr) {
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    host_.channel.reset();
    host_.primary_stash.reset();
  }
  host_.pending.reset();
  host_.detached_identity = {};
  host_.link_down = false;
  host_.announce_reattach = false;
}

bool TerminalSession::host_pending() const {
  std::scoped_lock lock(mutex_);
  return host_.pending.has_value();
}

void TerminalSession::SetPendingStatus(std::string_view text) {
  {
    std::scoped_lock lock(mutex_);
    if (!host_.pending.has_value()) {
      return;
    }
    lines_ = {MakeStatusLine(text, /*dim=*/false)};
    host_.screen_lines = lines_.size();
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

bool TerminalSession::StartPendingOnHost(const platform::ProcessLauncher& launcher) {
  std::optional<TerminalHostMode::PendingStart> pending;
  {
    std::scoped_lock lock(mutex_);
    pending.swap(host_.pending);
  }
  if (!pending.has_value()) {
    return false;
  }
  return StartArgv(launcher, pending->working_directory, pending->command,
                   std::move(pending->shell_argv), std::move(pending->attach));
}

HostIdentity TerminalSession::host_identity() const {
  std::shared_ptr<TerminalHostChannel> channel;
  {
    std::scoped_lock lock(mutex_);
    if (host_.pending.has_value()) {
      return host_.pending->attach;
    }
    if (host_.channel == nullptr) {
      return host_.detached_identity;
    }
    channel = host_.channel;
  }
  // Outside the session lock: the channel holds its own while it applies a frame,
  // which takes this one (lock order).
  return channel->identity();
}

HostReattach TerminalSession::host_reattach() const {
  const std::shared_ptr<TerminalHostChannel> channel = HostChannel();
  return channel != nullptr ? channel->reattach() : HostReattach::None;
}

void TerminalSession::DetachFromHost() {
  const std::shared_ptr<TerminalHostChannel> channel = HostChannel();
  if (channel == nullptr) {
    return;
  }
  HostIdentity identity = channel->identity();
  // Outside the lock, as Stop closes: Detach waits out a frame being applied.
  channel->Detach();
  {
    std::scoped_lock lock(mutex_);
    if (host_.channel != channel) {
      return;  // stopped or restarted meanwhile
    }
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    host_.channel.reset();
    running_ = false;
    host_.detached_identity = std::move(identity);
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

bool TerminalSession::host_shell_exited() const {
  std::scoped_lock lock(mutex_);
  return host_.channel != nullptr && host_.mirrored && !running_ && !host_.link_down;
}

void TerminalSession::NoteHostReattached() {
  std::scoped_lock lock(mutex_);
  host_.announce_reattach = true;
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
    if (host_.channel == nullptr) {
      return false;
    }
    channel = host_.channel;
    event.seq = host_.next_input_seq++;
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
    if (host_.channel == channel) {
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

namespace {

// Client side: rewrites a host frame's link ids into this session's table.
template <typename Intern>
void RemapHostLinks(TerminalHostFrame& frame, Intern&& intern) {
  // Host ids name the HOST's table; the lines get this session's ids for the
  // same URIs. A cell naming an id the frame did not define loses its link.
  std::vector<std::uint16_t> local(frame.links.size());
  for (std::size_t i = 0; i < frame.links.size(); ++i) {
    local[i] = intern(frame.links[i].uri);
  }
  const auto remap = [&](std::vector<TerminalLine>& lines) {
    for (TerminalLine& line : lines) {
      for (TerminalCell& cell : line.cells) {
        if (cell.link == 0) {
          continue;
        }
        const auto found = std::ranges::lower_bound(frame.links, cell.link, {}, &TerminalHostLink::id);
        cell.link = found != frame.links.end() && found->id == cell.link
                        ? local[static_cast<std::size_t>(found - frame.links.begin())]
                        : 0;
      }
    }
  };
  remap(frame.scrollback_lines);
  remap(frame.screen_lines);
}

}  // namespace

bool TerminalSession::ApplyHostFrame(TerminalHostFrame frame) {
  bool consistent = true;
  {
    std::scoped_lock lock(mutex_);
    if (host_.channel == nullptr) {
      return true;  // stopped: a late frame has nothing to apply to
    }
    // Predictions out first: Keep and Promote name CONFIRMED lines.
    prediction_.Withdraw(PredictionViewLocked());
    RemapHostLinks(frame, [this](std::string_view uri) { return InternLinkLocked(uri); });
    const bool alternate = frame.has(TerminalHostFrame::kAlternateScreen);
    if (frame.has(TerminalHostFrame::kReset)) {
      prediction_.Clear();
      // A cold attach: nothing held is valid, scrollback included.
      lines_.clear();
      host_.screen_lines = 0;
      host_.primary_stash.reset();
      host_.primary_stash_screen_lines = 0;
      host_.alternate = false;
      host_.screen_top = frame.previous_top;
      host_.history.gaps.clear();
      host_.history.first_line = frame.previous_top;
      host_.history.exhausted = false;
      // Rows above a mirror's first line can arrive later (RequestOlderHostHistory),
      // and each takes the trim total one DOWN: start it where that cannot reach 0.
      scrollback_trim_total_ = std::max(scrollback_trim_total_, TerminalHostHistory::kRowBase);
    }
    if (alternate && !host_.alternate) {
      host_.primary_stash = std::move(lines_);
      host_.primary_stash_screen_lines = host_.screen_lines;
      lines_.clear();
      host_.screen_lines = 0;
    } else if (!alternate && host_.alternate) {
      if (host_.primary_stash.has_value()) {
        lines_ = std::move(*host_.primary_stash);
      } else {
        lines_.clear();
      }
      host_.primary_stash.reset();
      host_.screen_lines = host_.primary_stash_screen_lines;
    }
    host_.alternate = alternate;

    // Detach the old screen from the tail; what stays is scrollback.
    std::vector<TerminalLine> old_screen;
    old_screen.reserve(host_.screen_lines);
    const std::size_t screen_from = lines_.size() - std::min(host_.screen_lines, lines_.size());
    for (std::size_t i = screen_from; i < lines_.size(); ++i) {
      old_screen.push_back(std::move(lines_[i]));
    }
    lines_.resize(screen_from);
    const std::uint64_t old_top = alternate ? 0 : host_.screen_top;
    const auto take_old = [&](std::uint64_t absolute, TerminalLine& out) {
      if (absolute < old_top || absolute - old_top >= old_screen.size()) {
        consistent = false;
        out = TerminalLine{};
        return;
      }
      out = std::move(old_screen[static_cast<std::size_t>(absolute - old_top)]);
    };

    if (!alternate) {
      if (frame.previous_top != host_.screen_top) {
        consistent = false;
      }
      if (frame.screen_top < frame.previous_top) {
        // The screen grew upward (a taller window): its new top rows were our
        // scrollback, and they come again as screen rows. `back` counts HOST
        // lines; a gap rule at the tail stands for many of them.
        std::uint64_t back = frame.previous_top - frame.screen_top;
        while (back > 0 && !lines_.empty()) {
          const std::uint64_t row = scrollback_trim_total_ + lines_.size() - 1;
          if (!host_.history.gaps.empty() && host_.history.gaps.back().row == row) {
            TerminalHostHistory::Gap& gap = host_.history.gaps.back();
            if (gap.count > back) {
              // Only the newest of the withheld lines are back on screen.
              gap.count -= back;
              lines_.back() = MakeStatusLine(GapText(gap.count), /*dim=*/true);
              break;
            }
            back -= gap.count;
            host_.history.gaps.pop_back();
          } else {
            --back;
          }
          lines_.pop_back();
        }
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
            host_.history.AddGap(scrollback_trim_total_ + lines_.size(), run.count);
            lines_.push_back(MakeStatusLine(GapText(run.count), /*dim=*/true));
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
    host_.screen_lines = std::max<std::size_t>(1, frame.screen_keep.size());
    if (!alternate) {
      host_.screen_top = frame.screen_top;
    }
    host_.mirrored = true;
    host_.link_down = false;

    rows_ = frame.rows;
    columns_ = frame.columns;
    // The client's own scrollback cap; the host's is the host's business.
    if (!alternate) {
      const std::size_t keep = max_scrollback_lines_ + host_.screen_lines;
      if (lines_.size() > keep + keep / 4) {
        const std::size_t trim = lines_.size() - keep;
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim));
        scrollback_trim_total_ += trim;
        // The front moves by host lines, not rows: a trimmed rule was `count` of them.
        std::uint64_t host_lines = trim;
        std::erase_if(host_.history.gaps, [&](const TerminalHostHistory::Gap& gap) {
          if (gap.row >= scrollback_trim_total_) {
            return false;
          }
          host_lines = host_lines + gap.count - 1;
          return true;
        });
        host_.history.first_line += host_lines;
      }
    }
    cursor_row_ = lines_.size() - host_.screen_lines + frame.cursor_row;
    cursor_column_ = frame.cursor_column;
    if (host_.announce_reattach && frame.has(TerminalHostFrame::kReset)) {
      // The recorded terminal is back: say so under the history it brought (VS
      // Code's "History restored"). On the alternate screen there is no history
      // on show to put it under.
      host_.announce_reattach = false;
      if (!alternate) {
        InsertHostRule(lines_, host_, scrollback_trim_total_, cursor_row_,
                       MakeStatusLine("\xe2\x8b\xaf reattached \xe2\x8b\xaf", /*dim=*/true));
      }
    }
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
    if (frame.notification) {
      pending_notification_ = Notification{.title = std::move(frame.notification->title),
                                           .body = std::move(frame.notification->body)};
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
    if (host_.channel == nullptr || !running_) {
      return;
    }
    running_ = false;
    host_.link_down = true;
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    InsertHostRule(lines_, host_, scrollback_trim_total_, cursor_row_,
                   MakeStatusLine("[connection to the host lost: " + std::string(reason) + "]", /*dim=*/false));
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

std::optional<TerminalSession::HostResumePoint> TerminalSession::host_resume_point() const {
  std::scoped_lock lock(mutex_);
  if (host_.channel == nullptr || !host_.mirrored) {
    return std::nullopt;
  }
  return HostResumePoint{.screen_top = host_.screen_top, .alternate = host_.alternate};
}

void TerminalSession::ReattachHost(std::shared_ptr<TerminalHostChannel> channel) {
  {
    std::scoped_lock lock(mutex_);
    prediction_.Withdraw(PredictionViewLocked());
    prediction_.Clear();
    host_.channel = std::move(channel);
    // The new client numbers its input from 1 again; so does the host.
    host_.next_input_seq = 1;
    running_ = host_.channel != nullptr;
    host_.link_down = false;
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

void TerminalSession::RequestOlderHostHistory() {
  constexpr std::size_t kPageLines = 500;
  std::shared_ptr<TerminalHostChannel> channel;
  std::uint64_t before = 0;
  std::size_t count = 0;
  {
    std::scoped_lock lock(mutex_);
    if (host_.channel == nullptr || !host_.mirrored || use_alternate_screen_ ||
        host_.history.in_flight || host_.history.exhausted || host_.history.first_line == 0) {
      return;
    }
    // Never past this client's own cap: the next frame would trim it straight off.
    const std::size_t keep = max_scrollback_lines_ + host_.screen_lines;
    if (lines_.size() >= keep) {
      return;
    }
    count = std::min(kPageLines, keep - lines_.size());
    before = host_.history.first_line;
    channel = host_.channel;
    host_.history.in_flight = true;
  }
  const bool sent = channel->FetchHistory(
      before, count, [this](std::uint64_t first, std::vector<TerminalLine> lines) {
        PrependHostHistory(first, std::move(lines));
      });
  if (!sent) {
    std::scoped_lock lock(mutex_);
    host_.history.in_flight = false;
  }
}

void TerminalSession::PrependHostHistory(std::uint64_t first, std::vector<TerminalLine> lines) {
  {
    std::scoped_lock lock(mutex_);
    host_.history.in_flight = false;
    if (lines.empty()) {
      host_.history.exhausted = true;  // the host holds nothing older
      return;
    }
    // Only a page that still ends where this mirror begins, on the same screen.
    if (use_alternate_screen_ || first + lines.size() != host_.history.first_line ||
        lines.size() > scrollback_trim_total_) {
      return;
    }
    const std::size_t count = lines.size();
    lines_.insert(lines_.begin(), std::make_move_iterator(lines.begin()),
                  std::make_move_iterator(lines.end()));
    scrollback_trim_total_ -= count;  // every row already held keeps its absolute index
    cursor_row_ += count;
    host_.history.first_line = first;
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

void TerminalSession::RequestHostGapFill(std::size_t first_row, std::size_t rows) {
  constexpr std::size_t kPageLines = 500;
  std::shared_ptr<TerminalHostChannel> channel;
  std::uint64_t gap_id = 0;
  std::uint64_t before = 0;
  std::size_t count = 0;
  {
    std::scoped_lock lock(mutex_);
    if (host_.channel == nullptr || !host_.mirrored || use_alternate_screen_) {
      return;
    }
    // Never past this client's own cap: the next frame would trim it straight off.
    const std::size_t keep = max_scrollback_lines_ + host_.screen_lines;
    if (lines_.size() >= keep) {
      return;
    }
    const std::uint64_t first = scrollback_trim_total_ + first_row;
    const std::uint64_t end = first + rows;
    for (TerminalHostHistory::Gap& gap : host_.history.gaps) {
      if (gap.row >= end) {
        break;
      }
      if (gap.row < first || gap.count == 0 || gap.unfillable) {
        continue;
      }
      if (gap.fill_in_flight) {
        return;  // one fill at a time; the next frame asks for the next gap
      }
      // The newest of the withheld lines first: they sit right above what follows
      // the rule, so a partial fill reads on from the text below it.
      before = host_.history.HostIndexOfRow(gap.row, scrollback_trim_total_) + gap.count;
      count = static_cast<std::size_t>(
          std::min<std::uint64_t>({gap.count, kPageLines, keep - lines_.size()}));
      gap.fill_in_flight = true;
      gap_id = gap.id;
      channel = host_.channel;
      break;
    }
  }
  if (channel == nullptr) {
    return;
  }
  const bool sent = channel->FetchHistory(
      before, count, [this, gap_id, before](std::uint64_t first, std::vector<TerminalLine> lines) {
        FillHostGap(gap_id, before, first, std::move(lines));
      });
  if (!sent) {
    std::scoped_lock lock(mutex_);
    for (TerminalHostHistory::Gap& gap : host_.history.gaps) {
      if (gap.id == gap_id) {
        gap.fill_in_flight = false;
      }
    }
  }
}

void TerminalSession::FillHostGap(std::uint64_t gap_id, std::uint64_t before, std::uint64_t first,
                                  std::vector<TerminalLine> lines) {
  {
    std::scoped_lock lock(mutex_);
    const auto gap_it =
        std::find_if(host_.history.gaps.begin(), host_.history.gaps.end(),
                     [gap_id](const TerminalHostHistory::Gap& gap) { return gap.id == gap_id; });
    if (gap_it == host_.history.gaps.end()) {
      return;  // trimmed, or taken back on screen, while the request was out
    }
    gap_it->fill_in_flight = false;
    if (lines.empty()) {
      gap_it->unfillable = true;  // the host holds none of it any more
      return;
    }
    // Only a page that still ends where the gap does, on the same screen; a gap a
    // growing screen shrank since is asked again by the next frame that shows it.
    const std::uint64_t gap_end =
        host_.history.HostIndexOfRow(gap_it->row, scrollback_trim_total_) + gap_it->count;
    if (use_alternate_screen_ || before != gap_end || first + lines.size() != gap_end ||
        lines.size() > gap_it->count) {
      return;
    }
    const std::size_t at = static_cast<std::size_t>(gap_it->row - scrollback_trim_total_);
    const std::uint64_t gap_row = gap_it->row;
    const std::size_t count = lines.size();
    std::uint64_t added = 0;
    if (count == gap_it->count) {
      // All of it: the lines replace the rule.
      lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(at));
      lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at),
                    std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
      added = count - 1;
      host_.history.gaps.erase(gap_it);
    } else {
      // The newest part: below the rule, which now stands for what is still older.
      gap_it->count -= count;
      lines_[at] = MakeStatusLine(GapText(gap_it->count), /*dim=*/true);
      lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at + 1),
                    std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
      added = count;
    }
    for (TerminalHostHistory::Gap& gap : host_.history.gaps) {
      if (gap.row > gap_row) {
        gap.row += added;
      }
    }
    cursor_row_ += added;  // the cursor is on the screen, below every gap
    host_.history.RecordRowShift(gap_row, added);
    AdvanceSnapshotGenerationLocked();
  }
  PushWakeEvent();
}

std::uint64_t TerminalSession::RowShiftSequence() const {
  std::scoped_lock lock(mutex_);
  return host_.history.row_shift_sequence;
}

bool TerminalSession::RowShiftsSince(std::uint64_t sequence,
                                     std::vector<TerminalHostHistory::RowShift>& out) const {
  std::scoped_lock lock(mutex_);
  out.clear();
  const std::uint64_t held_from = host_.history.row_shift_sequence - host_.history.row_shifts.size();
  if (sequence < held_from) {
    return false;  // further behind than the log reaches
  }
  for (std::uint64_t i = sequence - held_from; i < host_.history.row_shifts.size(); ++i) {
    out.push_back(host_.history.row_shifts[static_cast<std::size_t>(i)]);
  }
  return true;
}

void TerminalSession::SetOutputObserver(std::function<void()> observer) {
  std::scoped_lock lock(mutex_);
  output_observer_ = std::move(observer);
}

std::uint64_t TerminalSession::OutputCount() const {
  std::scoped_lock lock(mutex_);
  return output_count_;
}

bool TerminalSession::HasUnreadOutput() const {
  std::shared_ptr<platform::TerminalBackend> backend;
  {
    std::scoped_lock lock(mutex_);
    backend = backend_;
  }
  return backend != nullptr && backend->HasUnreadOutput();
}

std::uint64_t TerminalSession::CaptureHostHistory(std::uint64_t before, std::size_t count,
                                                  std::vector<TerminalLine>& out) const {
  std::scoped_lock lock(mutex_);
  out.clear();
  // On the alternate screen the primary buffer waits in its saved state; its
  // absolute indexes are the same (only the primary is ever trimmed).
  const std::deque<TerminalLine>& primary = use_alternate_screen_ ? primary_screen_.lines : lines_;
  const std::uint64_t held_end = scrollback_trim_total_ + primary.size();
  const std::uint64_t end = std::min(before, held_end);
  if (end <= scrollback_trim_total_) {
    return end;
  }
  const std::uint64_t first =
      std::max<std::uint64_t>(scrollback_trim_total_, end > count ? end - count : 0);
  out.reserve(static_cast<std::size_t>(end - first));
  for (std::uint64_t absolute = first; absolute < end; ++absolute) {
    TerminalLine& line = out.emplace_back(primary[static_cast<std::size_t>(absolute - scrollback_trim_total_)]);
    for (TerminalCell& cell : line.cells) {
      cell.link = 0;
    }
  }
  return first;
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
  // A host session has no UI to consume a notification: the client does.
  header.notification.reset();
  if (pending_notification_) {
    header.notification = TerminalHostNotification{.title = std::move(pending_notification_->title),
                                                   .body = std::move(pending_notification_->body)};
    pending_notification_.reset();
  }

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
  out.output_count = output_count_;

  // The URIs of the links those lines carry; the frame builder sends the ones
  // its lines use. Most captures have none and pay one pass over the cells.
  header.links.clear();
  if (links_.size() != 0) {
    std::vector<bool> seen(TerminalLinkTable::kMaxLinks + 1, false);
    for (const TerminalLine& line : out.lines) {
      for (const TerminalCell& cell : line.cells) {
        if (cell.link != 0 && !seen[cell.link]) {
          seen[cell.link] = true;
          header.links.push_back(
              TerminalHostLink{.id = cell.link, .uri = std::string(links_.Uri(cell.link))});
        }
      }
    }
    std::ranges::sort(header.links, {}, &TerminalHostLink::id);
  }
}

}  // namespace microide::terminal
