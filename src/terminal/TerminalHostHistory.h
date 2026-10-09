#pragma once

#include <cstdint>
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
  };
  std::vector<Gap> gaps;
  // The host-absolute index of the first row on the primary screen: where an
  // older page (term/scrollback) ends. Exact across trims because of `gaps`.
  std::uint64_t first_line = 0;
  bool in_flight = false;  // a term/scrollback request is out
  bool exhausted = false;  // the host said it holds nothing older
  // Pages are prepended, taking the trim total DOWN: a mirror starts it here so
  // that can never reach 0.
  static constexpr std::uint64_t kRowBase = std::uint64_t{1} << 40;
};

}  // namespace microide::terminal
