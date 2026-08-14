#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "terminal/TerminalCell.h"
#include "util/PerformanceCounters.h"

namespace microide::terminal {

// Recycles the cell buffers of trimmed scrollback lines into the lines that
// replace them (TD-2026-08-14-231).
//
// Every new scrollback line's cell vector starts empty and grows to hold the
// line: one heap allocation per line of output, `terminal.feed_output`'s single
// largest site at 4,000 allocations / 1.49 MB for 4,000 lines, all of it
// `PutAsciiRunLocked -> ResizeLineLocked -> vector::_M_fill_insert`. At the other
// end of the deque the scrollback trim destroys a batch of fully grown cell
// vectors at exactly the same rate.
//
// Those are the same buffers. Handing a trimmed batch straight to the lines being
// created turns "free, then allocate and grow" into a rename.
//
// This is deliberately NOT the memory trade the two obvious fixes are. Reserving
// `columns_` on every line's first write costs one allocation per line but makes
// every scrollback line hold a full-width buffer forever; an unbounded pool
// retains storage nothing is waiting for. What this holds is the storage that was
// live one instruction earlier, handed to the lines that consume it next — so in
// the steady state the high-water mark does not move at all. The cap below exists
// only for the case where output STOPS right after a large trim and the pool has
// no consumer: past it, the memory goes back.
class TerminalLineBufferPool {
 public:
  // Retention ceiling for a pool with no consumer. One mebibyte is ~700 lines of
  // 80-column cells — comfortably more than a trim batch at the default 2,000-line
  // scrollback, and a bounded, statable per-session cost.
  static constexpr std::size_t kMaxPooledBytes = 1u << 20;
  // A second bound on the COUNT, because the byte cap alone would admit tens of
  // thousands of nearly empty buffers (18 bytes each) whose vector headers cost
  // more than their contents.
  static constexpr std::size_t kMaxPooledBuffers = 4096;

  // Rescue the cell buffers of [first, last), up to the caps. The lines are left
  // valid-but-empty for the caller to erase.
  template <typename Iterator>
  void RecycleRange(Iterator first, Iterator last) {
    for (Iterator it = first; it != last; ++it) {
      const std::size_t bytes = it->cells.capacity() * sizeof(TerminalCell);
      if (bytes == 0) {
        continue;  // nothing to rescue
      }
      if (buffers_.size() >= kMaxPooledBuffers || pooled_bytes_ + bytes > kMaxPooledBytes) {
        break;
      }
      it->cells.clear();  // clear() keeps the capacity, which is the whole point
      pooled_bytes_ += bytes;
      buffers_.push_back(std::move(it->cells));
    }
  }

  // A blank line, backed by a pooled buffer when one is available.
  TerminalLine Take() {
    TerminalLine line;
    if (buffers_.empty()) {
      util::AddPerformanceCounter(util::PerfCounterId::TerminalLineBufferPoolMisses);
      return line;
    }
    line.cells = std::move(buffers_.back());
    buffers_.pop_back();
    pooled_bytes_ -= std::min(pooled_bytes_, line.cells.capacity() * sizeof(TerminalCell));
    // The pool only ever holds cleared buffers; clearing again is free and means
    // a future RecycleRange caller cannot hand out live cells by mistake.
    line.cells.clear();
    util::AddPerformanceCounter(util::PerfCounterId::TerminalLineBufferPoolHits);
    return line;
  }

  std::size_t size() const noexcept { return buffers_.size(); }
  std::size_t pooled_bytes() const noexcept { return pooled_bytes_; }

 private:
  std::vector<std::vector<TerminalCell>> buffers_;
  std::size_t pooled_bytes_ = 0;
};

}  // namespace microide::terminal
