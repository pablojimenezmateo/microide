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

// The clips are logical rects, and SDL scales one to device pixels as
// floor(x * s) wide by ceil(w * s) -- not out to ceil((x + w) * s). At a
// fractional scale (ui.scale 1.25, a 1.5x display) that stops a device column or
// row short of what a rect drawn at the same logical bounds fills, and the
// caret's last column stayed on screen through its blink (QA monkey, seed 1).
// One logical pixel more on every side covers it; at an integral scale the
// padding is returned unchanged.
render::TextClipPadding PadClipForPresentationScale(render::TextClipPadding padding,
                                                    float scale_x,
                                                    float scale_y);

// The logical region a clip actually reaches on the device -- the dirty hint the
// shell paints a clip with. The integer clip itself is not: at a fractional
// scale its device extent takes part of a pixel beyond it, and a surface that
// only touches the integer rect (the editor beside a sidebar clip) was skipped
// while its edge pixel inside the device clip was cleared.
SDL_FRect DeviceClipHint(const SDL_Rect& clip, float scale_x, float scale_y);

}  // namespace microide::app
