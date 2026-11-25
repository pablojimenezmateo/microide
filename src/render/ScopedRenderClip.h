#pragma once

#include <SDL3/SDL.h>

#include "render/DevicePixelSnap.h"

#include <algorithm>
#include <cmath>

namespace microide::render {

namespace scoped_render_clip_detail {

using device_pixel_snap_detail::DeviceScale;
using device_pixel_snap_detail::DevicePixelAlignment;
using device_pixel_snap_detail::kMaxDevicePixelAlignment;
using device_pixel_snap_detail::SnapOutward;
using device_pixel_snap_detail::Span;

inline Span NarrowAxis(int outer_start, int outer_length, int start, int length, float scale) {
  const auto device_start = [scale](int value) {
    return static_cast<int>(std::floor(static_cast<float>(value) * scale));
  };
  const auto device_end = [&](int value, int span) {
    return device_start(value) + static_cast<int>(std::ceil(static_cast<float>(span) * scale));
  };
  const int outer_device_end = device_end(outer_start, outer_length);
  const int target_start = std::max(device_start(outer_start), device_start(start));
  const int target_end = std::min(outer_device_end, device_end(start, length));
  if (target_end <= target_start) {
    return Span{std::max(outer_start, start), 0};
  }
  const int outer_end = outer_start + outer_length;
  const auto covering = [&](int from) {
    int span = std::max(0, static_cast<int>(std::floor(static_cast<float>(target_end - device_start(from)) / scale)) - 1);
    while (span <= outer_end - from && device_end(from, span) < target_end) {
      ++span;
    }
    return span > outer_end - from || device_end(from, span) > outer_device_end ? -1 : span;
  };
  const int narrowed = std::max(outer_start, start);
  for (int from = narrowed; from >= outer_start && from > narrowed - 8; --from) {
    if (const int span = covering(from); span >= 0) {
      return Span{from, span};
    }
  }
  return Span{outer_start, outer_length};
}

}

inline SDL_Rect AlignClipToDevicePixels(SDL_Renderer* renderer, const SDL_Rect& rect) {
  return SnapToDevicePixels(renderer, SDL_FRect{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                                static_cast<float>(rect.w), static_cast<float>(rect.h)});
}

inline SDL_Rect NarrowToCurrentClip(SDL_Renderer* renderer, const SDL_Rect& requested) {
  const SDL_Rect clip = AlignClipToDevicePixels(renderer, requested);
  if (!SDL_RenderClipEnabled(renderer)) {
    return clip;
  }
  SDL_Rect outer{};
  SDL_GetRenderClipRect(renderer, &outer);
  const SDL_FPoint scale = scoped_render_clip_detail::DeviceScale(renderer);
  const auto x = scoped_render_clip_detail::NarrowAxis(outer.x, outer.w, clip.x, clip.w, scale.x);
  const auto y = scoped_render_clip_detail::NarrowAxis(outer.y, outer.h, clip.y, clip.h, scale.y);
  if (x.length == 0 || y.length == 0) {
    return SDL_Rect{outer.x, outer.y, 0, 0};
  }
  return SDL_Rect{x.start, y.start, x.length, y.length};
}

class ScopedRenderClip {
 public:
  ScopedRenderClip(SDL_Renderer* renderer, const SDL_FRect& pane)
      : ScopedRenderClip(renderer, SDL_Rect{static_cast<int>(std::floor(pane.x)),
                                            static_cast<int>(std::floor(pane.y)),
                                            static_cast<int>(std::ceil(pane.w)),
                                            static_cast<int>(std::ceil(pane.h))}) {}
  ScopedRenderClip(SDL_Renderer* renderer, const SDL_Rect& clip) : renderer_(renderer) {
    had_clip_ = SDL_RenderClipEnabled(renderer_);
    if (had_clip_) {
      SDL_GetRenderClipRect(renderer_, &previous_clip_);
    }
    const SDL_Rect narrowed = NarrowToCurrentClip(renderer_, clip);
    SDL_SetRenderClipRect(renderer_, &narrowed);
  }

  ~ScopedRenderClip() {
    if (had_clip_) {
      SDL_SetRenderClipRect(renderer_, &previous_clip_);
    } else {
      SDL_SetRenderClipRect(renderer_, nullptr);
    }
  }

  ScopedRenderClip(const ScopedRenderClip&) = delete;
  ScopedRenderClip& operator=(const ScopedRenderClip&) = delete;

 private:
  SDL_Renderer* renderer_;
  bool had_clip_ = false;
  SDL_Rect previous_clip_{};
};

}
