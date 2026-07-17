#include "app/DirtyRegionPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace microide::app {
namespace {

constexpr int kDirtyRegionMergeGapPixels = 1;
constexpr std::size_t kPromoteWideCoverageMergedClipThreshold = 3;
constexpr std::size_t kPromoteFragmentedMergedClipThreshold = 6;
constexpr float kPromoteWideCoverageThreshold = 0.45f;

std::optional<SDL_Rect> ToClipRect(const SDL_FRect& rect,
                                   const render::TextClipPadding& padding,
                                   int width,
                                   int height) {
  // Reject non-finite or non-positive-size input BEFORE any arithmetic or float->int
  // cast: NaN/inf flow through floor/ceil and the subsequent static_cast<int> is UB
  // ([conv.fpint]). A negative/zero-size dirty rect is not a valid partial invalidation
  // either. SDL eventually consumes integer rects, so invalid geometry is dropped here.
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) ||
      !std::isfinite(rect.h) || rect.w <= 0.0f || rect.h <= 0.0f) {
    return std::nullopt;
  }

  // Compute padded edges and clamp to the surface bounds IN FLOAT space, so a huge
  // finite coordinate saturates to [0, width]/[0, height] rather than overflowing int
  // during the narrowing cast. width/height fit in int, so the clamped values do too.
  const float fw = static_cast<float>(width);
  const float fh = static_cast<float>(height);
  const float fx0 = std::clamp(std::floor(rect.x - padding.left), 0.0f, fw);
  const float fy0 = std::clamp(std::floor(rect.y - padding.top), 0.0f, fh);
  const float fx1 = std::clamp(std::ceil(rect.x + rect.w + padding.right), 0.0f, fw);
  const float fy1 = std::clamp(std::ceil(rect.y + rect.h + padding.bottom), 0.0f, fh);

  const int x0 = static_cast<int>(fx0);
  const int y0 = static_cast<int>(fy0);
  const int x1 = static_cast<int>(fx1);
  const int y1 = static_cast<int>(fy1);
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;
  }

  return SDL_Rect{.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0};
}

bool RectsOverlapOrNearlyTouch(const SDL_Rect& a, const SDL_Rect& b) {
  // Edge arithmetic in int64 so a near-INT_MAX coordinate (fuzzed or future
  // offscreen/high-DPI input) cannot overflow signed int before the comparison.
  // The normal AnalyzeDirtyRegions path already clamps rects to the surface, so
  // this only hardens the general helper.
  const std::int64_t a_right = static_cast<std::int64_t>(a.x) + a.w;
  const std::int64_t b_right = static_cast<std::int64_t>(b.x) + b.w;
  const std::int64_t a_bottom = static_cast<std::int64_t>(a.y) + a.h;
  const std::int64_t b_bottom = static_cast<std::int64_t>(b.y) + b.h;
  return a.x <= b_right + kDirtyRegionMergeGapPixels &&
         b.x <= a_right + kDirtyRegionMergeGapPixels &&
         a.y <= b_bottom + kDirtyRegionMergeGapPixels &&
         b.y <= a_bottom + kDirtyRegionMergeGapPixels;
}

SDL_Rect UnionRects(const SDL_Rect& a, const SDL_Rect& b) {
  // Compute edges in int64 then clamp back to the int range so a pathological
  // union can never wrap the resulting SDL_Rect width/height.
  constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
  constexpr std::int64_t kIntMin = std::numeric_limits<int>::min();
  const std::int64_t left = std::min(a.x, b.x);
  const std::int64_t top = std::min(a.y, b.y);
  const std::int64_t right =
      std::max(static_cast<std::int64_t>(a.x) + a.w, static_cast<std::int64_t>(b.x) + b.w);
  const std::int64_t bottom =
      std::max(static_cast<std::int64_t>(a.y) + a.h, static_cast<std::int64_t>(b.y) + b.h);
  const auto clamp_int = [kIntMin, kIntMax](std::int64_t value) {
    return static_cast<int>(std::clamp(value, kIntMin, kIntMax));
  };
  return SDL_Rect{.x = clamp_int(left),
                  .y = clamp_int(top),
                  .w = clamp_int(right - left),
                  .h = clamp_int(bottom - top)};
}

}  // namespace

DirtyRegionAnalysis AnalyzeDirtyRegions(const std::vector<SDL_FRect>& dirty_rects,
                                        const render::TextClipPadding& padding,
                                        int width,
                                        int height) {
  DirtyRegionAnalysis analysis;
  if (width <= 0 || height <= 0) {
    return analysis;
  }

  for (const SDL_FRect& dirty_rect : dirty_rects) {
    const auto clip_rect = ToClipRect(dirty_rect, padding, width, height);
    if (clip_rect.has_value()) {
      analysis.merged_clip_rects.push_back(*clip_rect);
    }
  }

  analysis.clipped_rect_count = analysis.merged_clip_rects.size();
  if (analysis.merged_clip_rects.empty()) {
    return analysis;
  }

  // Iterative merge: O(n²) worst case but n is always small in practice (< 20).
  // Uses swap+pop_back instead of mid-vector erase to keep each removal O(1).
  bool merged_any = true;
  while (merged_any) {
    merged_any = false;
    for (std::size_t i = 0; i < analysis.merged_clip_rects.size(); ++i) {
      for (std::size_t j = i + 1; j < analysis.merged_clip_rects.size(); ++j) {
        if (!RectsOverlapOrNearlyTouch(analysis.merged_clip_rects[i],
                                       analysis.merged_clip_rects[j])) {
          continue;
        }
        analysis.merged_clip_rects[i] =
            UnionRects(analysis.merged_clip_rects[i], analysis.merged_clip_rects[j]);
        if (j + 1 < analysis.merged_clip_rects.size()) {
          analysis.merged_clip_rects[j] = analysis.merged_clip_rects.back();
        }
        analysis.merged_clip_rects.pop_back();
        merged_any = true;
        break;
      }
      if (merged_any) {
        break;
      }
    }
  }

  std::sort(analysis.merged_clip_rects.begin(), analysis.merged_clip_rects.end(),
            [](const SDL_Rect& lhs, const SDL_Rect& rhs) {
              if (lhs.y != rhs.y) {
                return lhs.y < rhs.y;
              }
              if (lhs.x != rhs.x) {
                return lhs.x < rhs.x;
              }
              if (lhs.h != rhs.h) {
                return lhs.h < rhs.h;
              }
              return lhs.w < rhs.w;
            });

  const std::uint64_t scene_area =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  std::uint64_t merged_area = 0;
  for (const SDL_Rect& rect : analysis.merged_clip_rects) {
    merged_area += static_cast<std::uint64_t>(rect.w) * static_cast<std::uint64_t>(rect.h);
  }
  if (scene_area > 0) {
    merged_area = std::min(merged_area, scene_area);
    analysis.coverage = static_cast<float>(merged_area) / static_cast<float>(scene_area);
  }

  return analysis;
}

bool ShouldPromotePartialFrameToFull(const DirtyRegionAnalysis& analysis) {
  const std::size_t merged_clip_count = analysis.merged_clip_rects.size();
  if (merged_clip_count >= kPromoteFragmentedMergedClipThreshold) {
    return true;
  }

  return merged_clip_count >= kPromoteWideCoverageMergedClipThreshold &&
         analysis.coverage >= kPromoteWideCoverageThreshold;
}

}  // namespace microide::app
