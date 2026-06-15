#include "app/RedrawTraceAccumulator.h"

#include <algorithm>

namespace microide::app {

void RedrawTraceAccumulator::Configure(bool enabled, bool verbose) {
  enabled_ = enabled;
  verbose_ = verbose;
}

void RedrawTraceAccumulator::Record(const RedrawFrameStats& stats) {
  const char* reason = stats.reason != nullptr ? stats.reason : "unknown";
  if (verbose_) {
    SDL_Log(
        "microide redraw frame: reason=%s requested=%s rendered=%s promoted=%s dirty=%zu clips=%zu elapsed=%.2f ms",
        reason, stats.full_redraw_requested ? "full" : "partial",
        stats.full_redraw ? "full" : "partial", stats.promoted_partial_to_full ? "yes" : "no",
        stats.dirty_rect_count, stats.rendered_clip_count,
        static_cast<double>(stats.elapsed_ns) / 1'000'000.0);
  }

  if (!enabled_) {
    return;
  }

  ++frames_;
  total_ns_ += stats.elapsed_ns;
  total_dirty_rects_ += stats.dirty_rect_count;
  total_rendered_clips_ += stats.rendered_clip_count;
  max_dirty_rects_ = std::max(max_dirty_rects_, stats.dirty_rect_count);
  max_rendered_clips_ = std::max(max_rendered_clips_, stats.rendered_clip_count);
  last_reason_ = reason;
  if (stats.full_redraw || stats.dirty_rect_count == 0) {
    ++full_frames_;
  } else {
    ++partial_frames_;
  }

  if (frames_ < kLogInterval) {
    return;
  }
  Flush();
  Reset();
}

void RedrawTraceAccumulator::Flush() {
  const double average_ms =
      static_cast<double>(total_ns_) / static_cast<double>(frames_) / 1'000'000.0;
  const double average_dirty_rects =
      frames_ == 0 ? 0.0
                   : static_cast<double>(total_dirty_rects_) / static_cast<double>(frames_);
  const double average_rendered_clips =
      partial_frames_ == 0
          ? 0.0
          : static_cast<double>(total_rendered_clips_) / static_cast<double>(partial_frames_);
  SDL_Log(
      "microide redraw: %llu frames | %llu full | %llu partial | avg %.2f ms | avg dirty %.2f "
      "| avg clips %.2f | max dirty %zu | max clips %zu | last=%s",
      static_cast<unsigned long long>(frames_), static_cast<unsigned long long>(full_frames_),
      static_cast<unsigned long long>(partial_frames_), average_ms, average_dirty_rects,
      average_rendered_clips, max_dirty_rects_, max_rendered_clips_,
      last_reason_ != nullptr ? last_reason_ : "unknown");
}

void RedrawTraceAccumulator::Reset() {
  frames_ = 0;
  full_frames_ = 0;
  partial_frames_ = 0;
  total_ns_ = 0;
  total_dirty_rects_ = 0;
  total_rendered_clips_ = 0;
  max_dirty_rects_ = 0;
  max_rendered_clips_ = 0;
}

}  // namespace microide::app
