#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace microide::terminal {

// A host-terminal mirror's map from its rows to the host's lines
// (dev-docs/design/remote-projects.md § 6.12), kept by TerminalSession under its lock.
struct TerminalHostHistory {
  // A scrollback row that stands for `count` host lines: a Gap rule (withheld
  // lines), or 0 for a note of this side's own ("connection lost"). Rows are
  // absolute (scrollback trim total + deque row), ascending. A screen that grows
  // upward takes host lines back off the tail, and a rule there is `count` of them.
  struct Gap {
    std::uint64_t row = 0;
    std::uint64_t count = 0;
    // Names the gap to a term/scrollback answer, which can arrive after other
    // fills have moved it (its `row` is not stable, this is).
    std::uint64_t id = 0;
    bool fill_in_flight = false;
    // The host answered with nothing: it no longer holds these lines (trimmed
    // while detached), so asking again would only ask again.
    bool unfillable = false;
  };
  std::vector<Gap> gaps;
  std::uint64_t next_gap_id = 1;
  // The host-absolute index of the first row on the primary screen: where an
  // older page (term/scrollback) ends. Exact across trims because of `gaps`.
  std::uint64_t first_line = 0;
  bool in_flight = false;  // a term/scrollback request is out
  bool exhausted = false;  // the host said it holds nothing older
  // Pages are prepended, taking the trim total DOWN: a mirror starts it here so
  // that can never reach 0.
  static constexpr std::uint64_t kRowBase = std::uint64_t{1} << 40;

  // Rows inserted into the middle of the buffer: every row whose absolute index is
  // above `after_row` moved down by `added`. A trim or a prepended page moves no
  // absolute index (the trim total carries those), but filling a gap does, and a
  // view anchored on rows below it has to follow. Kept for the last few fills; a
  // view further behind than that re-anchors instead.
  struct RowShift {
    std::uint64_t after_row = 0;
    std::uint64_t added = 0;
  };
  static constexpr std::size_t kMaxRowShifts = 64;
  std::deque<RowShift> row_shifts;
  // How many shifts have ever been recorded; row_shifts holds the newest of them.
  std::uint64_t row_shift_sequence = 0;

  void AddGap(std::uint64_t row, std::uint64_t count) {
    gaps.push_back(Gap{.row = row, .count = count, .id = next_gap_id++});
  }

  // The host-absolute index of the host line a mirror row shows, or, for a gap
  // row, of the first line it withheld. `trim_total` is the session's.
  std::uint64_t HostIndexOfRow(std::uint64_t absolute_row, std::uint64_t trim_total) const {
    // Each earlier row stands for one host line, except a gap row (its count) and a
    // note (none): the offset is the row distance plus every earlier gap's excess.
    std::int64_t index = static_cast<std::int64_t>(first_line) +
                         static_cast<std::int64_t>(absolute_row - trim_total);
    for (const Gap& gap : gaps) {
      if (gap.row >= absolute_row) {
        break;
      }
      index += static_cast<std::int64_t>(gap.count) - 1;
    }
    return static_cast<std::uint64_t>(index);
  }

  void RecordRowShift(std::uint64_t after_row, std::uint64_t added) {
    if (added == 0) {
      return;
    }
    row_shifts.push_back(RowShift{.after_row = after_row, .added = added});
    ++row_shift_sequence;
    while (row_shifts.size() > kMaxRowShifts) {
      row_shifts.pop_front();
    }
  }
};

}  // namespace microide::terminal
