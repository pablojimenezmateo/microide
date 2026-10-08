#include "terminal/TerminalHostFrameBuilder.h"

#include <limits>

namespace microide::terminal {

void TerminalHostFrameBuilder::Reset() {
  reset_ = true;
  alternate_ = false;
  primary_top_ = 0;
  shadow_top_ = 0;
  shadow_.clear();
  generation_ = 0;
  title_.clear();
  working_directory_.clear();
}

std::size_t TerminalHostFrameBuilder::EstimateLineBytes(const TerminalLine& line) {
  // One byte per ASCII glyph plus a style run every few cells, plus the header.
  return 4 + line.cells.size() + line.cells.size() / 4;
}

bool TerminalHostFrameBuilder::UpToDate(const TerminalSession::HostCapture& capture) const {
  return !reset_ && capture.generation == generation_ && !capture.header.clipboard &&
         capture.header.title.value_or(std::string()) == title_ &&
         capture.header.working_directory.value_or(std::string()) == working_directory_;
}

std::size_t TerminalHostFrameBuilder::Build(const TerminalSession::HostCapture& capture,
                                            std::size_t scrollback_budget_bytes,
                                            TerminalHostFrame& frame) {
  const TerminalHostFrame& header = capture.header;
  frame.rows = header.rows;
  frame.columns = header.columns;
  frame.flags = header.flags;
  frame.cursor_shape = header.cursor_shape;
  frame.cursor_row = header.cursor_row;
  frame.cursor_column = header.cursor_column;
  frame.scrollback.clear();
  frame.scrollback_lines.clear();
  frame.screen_keep.clear();
  frame.screen_lines.clear();
  frame.clipboard = header.clipboard;

  const bool alternate = header.has(TerminalHostFrame::kAlternateScreen);
  // The shadow describes the client's screen only while it is the same screen:
  // after a switch the client's "old screen" is a different buffer.
  const bool shadow_valid = !reset_ && alternate == alternate_;
  if (reset_) {
    frame.flags |= TerminalHostFrame::kReset;
  }
  const std::uint64_t first = capture.first_line;
  const std::uint64_t end = first + capture.lines.size();
  const std::uint64_t screen_top = alternate ? 0 : header.screen_top;
  const auto shadow_has = [&](std::uint64_t absolute, const TerminalLine& line) {
    return shadow_valid && absolute >= shadow_top_ && absolute - shadow_top_ < shadow_.size() &&
           SameLine(shadow_[static_cast<std::size_t>(absolute - shadow_top_)], line);
  };

  std::size_t sent_bytes = 0;
  if (alternate) {
    // No scrollback on the alternate screen; the primary position stands.
    frame.previous_top = primary_top_;
    frame.screen_top = primary_top_;
  } else {
    // A cold attach starts where the capture does: the prefetched history.
    const std::uint64_t previous = reset_ ? std::min(first, screen_top) : primary_top_;
    frame.previous_top = previous;
    frame.screen_top = screen_top;
    const auto append = [&](TerminalHostRun::Kind kind) {
      if (!frame.scrollback.empty() && frame.scrollback.back().kind == kind) {
        ++frame.scrollback.back().count;
      } else {
        frame.scrollback.push_back(TerminalHostRun{.kind = kind, .count = 1});
      }
    };
    for (std::uint64_t absolute = previous; absolute < screen_top; ++absolute) {
      if (absolute < first) {
        append(TerminalHostRun::Kind::Gap);  // trimmed on the host, or past the cap
        continue;
      }
      const TerminalLine& line = capture.lines[static_cast<std::size_t>(absolute - first)];
      if (shadow_has(absolute, line)) {
        append(TerminalHostRun::Kind::Promote);
        continue;
      }
      const std::size_t bytes = EstimateLineBytes(line);
      if (sent_bytes + bytes > scrollback_budget_bytes) {
        append(TerminalHostRun::Kind::Gap);
        continue;
      }
      sent_bytes += bytes;
      frame.scrollback_lines.push_back(line);
      append(TerminalHostRun::Kind::Lines);
    }
  }

  for (std::uint64_t absolute = std::max(screen_top, first); absolute < end; ++absolute) {
    const TerminalLine& line = capture.lines[static_cast<std::size_t>(absolute - first)];
    const bool keep = shadow_has(absolute, line);
    frame.screen_keep.push_back(keep);
    if (!keep) {
      frame.screen_lines.push_back(line);
    }
  }

  // What the client now holds.
  shadow_top_ = screen_top;
  const std::size_t screen_from =
      static_cast<std::size_t>(std::min(end, std::max(screen_top, first)) - first);
  shadow_.resize(capture.lines.size() - screen_from);
  for (std::size_t i = 0; i < shadow_.size(); ++i) {
    shadow_[i].cells.assign(capture.lines[screen_from + i].cells.begin(),
                            capture.lines[screen_from + i].cells.end());
    shadow_[i].wrapped_from_previous = capture.lines[screen_from + i].wrapped_from_previous;
  }
  if (!alternate) {
    primary_top_ = screen_top;
  }
  alternate_ = alternate;
  reset_ = false;
  generation_ = capture.generation;

  const std::string& title = header.title.value_or(std::string());
  frame.title.reset();
  if (title != title_) {
    title_ = title;
    frame.title = title;
  }
  const std::string& cwd = header.working_directory.value_or(std::string());
  frame.working_directory.reset();
  if (cwd != working_directory_) {
    working_directory_ = cwd;
    frame.working_directory = cwd;
  }
  return sent_bytes;
}

}  // namespace microide::terminal
