#pragma once

#include <SDL3/SDL.h>

#include <cmath>

#include "editor/PluginSurfaceStore.h"

namespace microide::workspace {

inline constexpr float kPluginSurfacePreviewPadding = 8.0f;

inline float PluginSurfacePreviewContentHeight(const editor::SurfaceContent& content) {
  return content.intrinsic_height + 2.0f * kPluginSurfacePreviewPadding;
}

inline int MaxPluginSurfacePreviewScroll(const editor::SurfaceContent& content,
                                         float body_height) {
  const float overflow = PluginSurfacePreviewContentHeight(content) - body_height;
  return overflow > 0.0f ? static_cast<int>(std::ceil(overflow)) : 0;
}

inline SDL_FRect PluginSurfacePreviewHitRegionRect(const SDL_FRect& body, float scroll_y,
                                                   const editor::SurfaceHitRegion& region) {
  return SDL_FRect{body.x + kPluginSurfacePreviewPadding + region.rect.x,
                   body.y + kPluginSurfacePreviewPadding + region.rect.y - scroll_y, region.rect.w,
                   region.rect.h};
}

inline const editor::SurfaceHitRegion* FindPluginSurfacePreviewHitRegion(
    const editor::SurfaceContent& content,
    const SDL_FRect& body,
    float scroll_y,
    float x,
    float y) {
  const float content_x = x - (body.x + kPluginSurfacePreviewPadding);
  const float content_y = y - (body.y + kPluginSurfacePreviewPadding) + scroll_y;
  for (auto it = content.hit_regions.rbegin(); it != content.hit_regions.rend(); ++it) {
    const SDL_FRect& r = it->rect;
    if (r.w > 0.0f && r.h > 0.0f && content_x >= r.x && content_x < r.x + r.w &&
        content_y >= r.y && content_y < r.y + r.h) {
      return &*it;
    }
  }
  return nullptr;
}

}
