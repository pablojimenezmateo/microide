#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalInput.h"

namespace microide::terminal {

class TerminalHoverAffordance {
 public:
  void Probe(std::size_t row, std::size_t column, const TerminalLine& line);
  bool Contains(std::size_t row, std::size_t column, const TerminalLine& line) const;
  void Reset();

 private:
  bool Lit(std::size_t column, const TerminalLine& line) const;

  bool armed_ = false;
  std::size_t row_ = 0;
  std::size_t column_ = 0;
  std::vector<TerminalCell> before_;
};

struct TerminalPointerState {
  std::optional<TerminalMotionCell> last_motion;
  TerminalHoverAffordance hover;
};

}
