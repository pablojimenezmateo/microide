#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace microide::terminal {

struct TerminalHostHistory {
  struct Gap {
    std::uint64_t row = 0;
    std::uint64_t count = 0;
    std::uint64_t id = 0;
    bool fill_in_flight = false;
    bool unfillable = false;
  };
  std::vector<Gap> gaps;
  std::uint64_t next_gap_id = 1;
  std::uint64_t first_line = 0;
  bool in_flight = false;
  bool exhausted = false;
  static constexpr std::uint64_t kRowBase = std::uint64_t{1} << 40;

  struct RowShift {
    std::uint64_t after_row = 0;
    std::uint64_t added = 0;
  };
  static constexpr std::size_t kMaxRowShifts = 64;
  std::deque<RowShift> row_shifts;
  std::uint64_t row_shift_sequence = 0;

  void AddGap(std::uint64_t row, std::uint64_t count) {
    gaps.push_back(Gap{.row = row, .count = count, .id = next_gap_id++});
  }

  std::uint64_t HostIndexOfRow(std::uint64_t absolute_row, std::uint64_t trim_total) const {
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

}
