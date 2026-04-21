#include "app/DirtyRegionPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
  const int x0 = std::max(0, static_cast<int>(std::floor(rect.x - padding.left)));
  const int y0 = std::max(0, static_cast<int>(std::floor(rect.y - padding.top)));
  const int x1 = std::min(width,
                          static_cast<int>(std::ceil(rect.x + rect.w + padding.right)));
  const int y1 = std::min(height,
                          static_cast<int>(std::ceil(rect.y + rect.h + padding.bottom)));
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;
  }

  return SDL_Rect{.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0};
}

bool RectsOverlapOrNearlyTouch(const SDL_Rect& a, const SDL_Rect& b) {
  const int a_right = a.x + a.w;
  const int b_right = b.x + b.w;
  const int a_bottom = a.y + a.h;
  const int b_bottom = b.y + b.h;
  return a.x <= b_right + kDirtyRegionMergeGapPixels &&
         b.x <= a_right + kDirtyRegionMergeGapPixels &&
         a.y <= b_bottom + kDirtyRegionMergeGapPixels &&
         b.y <= a_bottom + kDirtyRegionMergeGapPixels;
}

SDL_Rect UnionRects(const SDL_Rect& a, const SDL_Rect& b) {
  const int left = std::min(a.x, b.x);
  const int top = std::min(a.y, b.y);
  const int right = std::max(a.x + a.w, b.x + b.w);
  const int bottom = std::max(a.y + a.h, b.y + b.h);
  return SDL_Rect{.x = left, .y = top, .w = right - left, .h = bottom - top};
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
