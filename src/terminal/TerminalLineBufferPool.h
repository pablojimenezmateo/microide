#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "terminal/TerminalCell.h"
#include "util/PerformanceCounters.h"

namespace microide::terminal {

class TerminalLineBufferPool {
 public:
  static constexpr std::size_t kMaxPooledBytes = 1u << 20;
  static constexpr std::size_t kMaxPooledBuffers = 4096;

  template <typename Iterator>
  void RecycleRange(Iterator first, Iterator last) {
    for (Iterator it = first; it != last; ++it) {
      const std::size_t bytes = it->cells.capacity() * sizeof(TerminalCell);
      if (bytes == 0) {
        continue;
      }
      if (buffers_.size() >= kMaxPooledBuffers || pooled_bytes_ + bytes > kMaxPooledBytes) {
        break;
      }
      it->cells.clear();
      pooled_bytes_ += bytes;
      buffers_.push_back(std::move(it->cells));
    }
  }

  TerminalLine Take() {
    TerminalLine line;
    if (buffers_.empty()) {
      util::AddPerformanceCounter(util::PerfCounterId::TerminalLineBufferPoolMisses);
      return line;
    }
    line.cells = std::move(buffers_.back());
    buffers_.pop_back();
    pooled_bytes_ -= std::min(pooled_bytes_, line.cells.capacity() * sizeof(TerminalCell));
    line.cells.clear();
    util::AddPerformanceCounter(util::PerfCounterId::TerminalLineBufferPoolHits);
    return line;
  }

  std::size_t size() const noexcept { return buffers_.size(); }

 private:
  std::vector<std::vector<TerminalCell>> buffers_;
  std::size_t pooled_bytes_ = 0;
};

}
