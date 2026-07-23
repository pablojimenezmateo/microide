#pragma once

#include <SDL3/SDL.h>

#include <cmath>

#include "editor/PluginSurfaceStore.h"

// Deterministic geometry helpers for the bottom-panel plugin surface preview
// (Phase E0). Shared by the render path, the mouse coordinator, the cursor
// resolver, and frame prep so scroll clamping and hit-region mapping agree
// byte-for-byte across all of them (TD-2026-07-16-60/61).
namespace microide::workspace {

// Content inset used when drawing a plugin surface preview body: the display
// list / raster origin sits at (body.x + pad, body.y + pad - scroll_y).
inline constexpr float kPluginSurfacePreviewPadding = 8.0f;

// Total scrollable content height of a preview: the surface's intrinsic height
// plus the symmetric padding.
inline float PluginSurfacePreviewContentHeight(const editor::SurfaceContent& content) {
  return content.intrinsic_height + 2.0f * kPluginSurfacePreviewPadding;
}

// Maximum scroll offset (pixels) for a preview shown in a body of height
// `body_height`. Zero when the content fits.
inline int MaxPluginSurfacePreviewScroll(const editor::SurfaceContent& content,
                                         float body_height) {
  const float overflow = PluginSurfacePreviewContentHeight(content) - body_height;
  return overflow > 0.0f ? static_cast<int>(std::ceil(overflow)) : 0;
}

// Topmost hit region under the panel-space point (x, y), or null. Regions are
// authored in content-local coordinates; the point is mapped through the body
// origin, the content padding, and the current scroll. Later-published regions
// paint on top, so the scan runs back-to-front (last match wins). Degenerate
// (non-positive size) regions never match. The caller guarantees the point is
// inside the preview body rect.
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

}  // namespace microide::workspace
