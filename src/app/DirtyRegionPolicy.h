#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <vector>

#include "render/TextRendererBackend.h"

namespace microide::app {

struct DirtyRegionAnalysis {
  std::vector<SDL_Rect> merged_clip_rects;
  std::size_t clipped_rect_count = 0;
  float coverage = 0.0f;
};

DirtyRegionAnalysis AnalyzeDirtyRegions(const std::vector<SDL_FRect>& dirty_rects,
                                        const render::TextClipPadding& padding,
                                        int width,
                                        int height);

bool ShouldPromotePartialFrameToFull(const DirtyRegionAnalysis& analysis);

}  // namespace microide::app
