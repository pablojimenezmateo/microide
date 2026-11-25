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
                                        int height,
                                        SDL_FPoint device_scale = SDL_FPoint{1.0f, 1.0f});

void AnalyzeDirtyRegions(const std::vector<SDL_FRect>& dirty_rects,
                         const render::TextClipPadding& padding,
                         int width,
                         int height,
                         SDL_FPoint device_scale,
                         DirtyRegionAnalysis& out);

bool ShouldPromotePartialFrameToFull(const DirtyRegionAnalysis& analysis);

render::TextClipPadding PadClipForPresentationScale(render::TextClipPadding padding,
                                                    float scale_x,
                                                    float scale_y);

SDL_FRect DeviceClipHint(const SDL_Rect& clip, float scale_x, float scale_y);

}
