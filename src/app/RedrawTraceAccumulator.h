#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

namespace microide::app {

// Per-frame render statistics handed to the trace accumulator.
struct RedrawFrameStats {
  bool full_redraw_requested = false;
  bool full_redraw = false;
  bool promoted_partial_to_full = false;
  std::size_t dirty_rect_count = 0;
  std::size_t rendered_clip_count = 0;
  const char* reason = nullptr;
  Uint64 elapsed_ns = 0;
};

// Accumulates redraw timing/coverage statistics and logs a rolling summary
// every `kLogInterval` frames when tracing is enabled (MICROIDE_TRACE_REDRAW).
// Verbose mode additionally logs every individual frame. This is pure
// bookkeeping extracted from Application so it can be unit-tested without SDL
// rendering.
class RedrawTraceAccumulator {
 public:
  static constexpr Uint64 kLogInterval = 120;

  RedrawTraceAccumulator() = default;

  // Reconfigure tracing flags (typically read from PerformanceTrace at startup).
  void Configure(bool enabled, bool verbose);

  bool enabled() const { return enabled_; }

  // Record one rendered frame: emits the verbose per-frame line when verbose
  // tracing is on, accumulates totals, and flushes a summary + resets once
  // `kLogInterval` frames have been seen.
  void Record(const RedrawFrameStats& stats);

  // Test accessors for the rolling counters (since the last flush).
  Uint64 frames() const { return frames_; }
  Uint64 full_frames() const { return full_frames_; }
  Uint64 partial_frames() const { return partial_frames_; }

 private:
  void Flush();
  void Reset();

  bool enabled_ = false;
  bool verbose_ = false;
  Uint64 frames_ = 0;
  Uint64 full_frames_ = 0;
  Uint64 partial_frames_ = 0;
  Uint64 total_ns_ = 0;
  Uint64 total_dirty_rects_ = 0;
  Uint64 total_rendered_clips_ = 0;
  std::size_t max_dirty_rects_ = 0;
  std::size_t max_rendered_clips_ = 0;
  const char* last_reason_ = nullptr;
};

}  // namespace microide::app
