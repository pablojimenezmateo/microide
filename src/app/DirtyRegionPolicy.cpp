#include "app/DirtyRegionPolicy.h"

#include "render/DevicePixelSnap.h"

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
                                   int height,
                                   SDL_FPoint device_scale) {
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) ||
      !std::isfinite(rect.h) || rect.w <= 0.0f || rect.h <= 0.0f) {
    return std::nullopt;
  }

  const float fw = static_cast<float>(width);
  const float fh = static_cast<float>(height);
  const float fx0 = std::clamp(rect.x - padding.left, 0.0f, fw);
  const float fy0 = std::clamp(rect.y - padding.top, 0.0f, fh);
  const float fx1 = std::clamp(rect.x + rect.w + padding.right, 0.0f, fw);
  const float fy1 = std::clamp(rect.y + rect.h + padding.bottom, 0.0f, fh);
  const SDL_Rect snapped =
      render::SnapToDevicePixels(SDL_FRect{fx0, fy0, fx1 - fx0, fy1 - fy0}, device_scale);
  const int x0 = std::max(snapped.x, 0);
  const int y0 = std::max(snapped.y, 0);
  const int x1 = std::min(snapped.x + snapped.w, width);
  const int y1 = std::min(snapped.y + snapped.h, height);
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;
  }

  return SDL_Rect{.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0};
}

bool RectsOverlapOrNearlyTouch(const SDL_Rect& a, const SDL_Rect& b) {
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

}

DirtyRegionAnalysis AnalyzeDirtyRegions(const std::vector<SDL_FRect>& dirty_rects,
                                        const render::TextClipPadding& padding,
                                        int width,
                                        int height,
                                        SDL_FPoint device_scale) {
  DirtyRegionAnalysis analysis;
  AnalyzeDirtyRegions(dirty_rects, padding, width, height, device_scale, analysis);
  return analysis;
}

void AnalyzeDirtyRegions(const std::vector<SDL_FRect>& dirty_rects,
                         const render::TextClipPadding& padding,
                         int width,
                         int height,
                         SDL_FPoint device_scale,
                         DirtyRegionAnalysis& analysis) {
  analysis.merged_clip_rects.clear();
  analysis.clipped_rect_count = 0;
  analysis.coverage = 0.0f;
  if (width <= 0 || height <= 0) {
    return;
  }

  for (const SDL_FRect& dirty_rect : dirty_rects) {
    const auto clip_rect = ToClipRect(dirty_rect, padding, width, height, device_scale);
    if (clip_rect.has_value()) {
      analysis.merged_clip_rects.push_back(*clip_rect);
    }
  }

  analysis.clipped_rect_count = analysis.merged_clip_rects.size();
  if (analysis.merged_clip_rects.empty()) {
    return;
  }

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
}

render::TextClipPadding PadClipForPresentationScale(render::TextClipPadding padding,
                                                    float scale_x,
                                                    float scale_y) {
  const auto fractional = [](float scale) {
    return std::isfinite(scale) && std::fabs(scale - std::round(scale)) > 1e-3f;
  };
  if (fractional(scale_x)) {
    padding.left += 1.0f;
    padding.right += 1.0f;
  }
  if (fractional(scale_y)) {
    padding.top += 1.0f;
    padding.bottom += 1.0f;
  }
  return padding;
}

SDL_FRect DeviceClipHint(const SDL_Rect& clip, float scale_x, float scale_y) {
  const auto valid = [](float scale) { return std::isfinite(scale) && scale > 0.0f; };
  if (!valid(scale_x) || !valid(scale_y)) {
    return SDL_FRect{static_cast<float>(clip.x), static_cast<float>(clip.y),
                     static_cast<float>(clip.w), static_cast<float>(clip.h)};
  }
  const float device_x = std::floor(static_cast<float>(clip.x) * scale_x);
  const float device_y = std::floor(static_cast<float>(clip.y) * scale_y);
  const float device_w = std::ceil(static_cast<float>(clip.w) * scale_x);
  const float device_h = std::ceil(static_cast<float>(clip.h) * scale_y);
  return SDL_FRect{device_x / scale_x, device_y / scale_y, device_w / scale_x, device_h / scale_y};
}

bool ShouldPromotePartialFrameToFull(const DirtyRegionAnalysis& analysis) {
  const std::size_t merged_clip_count = analysis.merged_clip_rects.size();
  if (merged_clip_count >= kPromoteFragmentedMergedClipThreshold) {
    return true;
  }

  return merged_clip_count >= kPromoteWideCoverageMergedClipThreshold &&
         analysis.coverage >= kPromoteWideCoverageThreshold;
}

}
