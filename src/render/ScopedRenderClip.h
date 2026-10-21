#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace microide::render {

namespace scoped_render_clip_detail {

// The scale SDL maps this view's logical coordinates to device pixels with: the
// logical presentation's (the window's, or the scene texture's own) times the
// user scale.
inline SDL_FPoint DeviceScale(SDL_Renderer* renderer) {
  SDL_FPoint scale{1.0f, 1.0f};
  int logical_w = 0;
  int logical_h = 0;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  if (SDL_GetRenderLogicalPresentation(renderer, &logical_w, &logical_h, &mode) &&
      mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_w > 0 && logical_h > 0) {
    SDL_FRect device{};
    if (SDL_GetRenderLogicalPresentationRect(renderer, &device) && device.w > 0.0f && device.h > 0.0f) {
      scale = SDL_FPoint{device.w / static_cast<float>(logical_w),
                         device.h / static_cast<float>(logical_h)};
    }
  }
  float user_x = 1.0f;
  float user_y = 1.0f;
  if (SDL_GetRenderScale(renderer, &user_x, &user_y)) {
    scale.x *= user_x;
    scale.y *= user_y;
  }
  return scale;
}

// One axis of the narrowing. SDL maps a clip's [start, start + length) to the
// device as [floor(start*s), floor(start*s) + ceil(length*s)) -- SDL 3.2's
// UpdatePixelClipRect -- so at a fractional scale the logical intersection is
// not the device intersection, and not every device span is a logical one. This
// returns a logical span starting exactly where the device intersection starts
// and reaching its end: rows the outer clip covers are painted (a row left out
// keeps whatever an earlier frame drew there). Where the end cannot be hit
// exactly it overshoots by at most one device row -- never before the start:
// starting the span at the surface's own edge instead painted a tab strip over
// the submenu above the outer clip (CI qa, menu-clicks).
struct Span {
  int start = 0;
  int length = 0;
};

inline Span NarrowAxis(int outer_start, int outer_length, int start, int length, float scale) {
  const auto device_start = [scale](int value) {
    return static_cast<int>(std::floor(static_cast<float>(value) * scale));
  };
  const auto device_end = [&](int value, int span) {
    return device_start(value) + static_cast<int>(std::ceil(static_cast<float>(span) * scale));
  };
  const int target_start = std::max(device_start(outer_start), device_start(start));
  const int target_end =
      std::min(device_end(outer_start, outer_length), device_end(start, length));
  if (target_end <= target_start) {
    return Span{std::max(outer_start, start), 0};
  }
  const int logical_end = start + length;
  // The shortest span from `from` whose device end reaches target_end.
  const auto covering_length = [&](int from) {
    int span = std::max(0, static_cast<int>(std::floor(static_cast<float>(target_end - device_start(from)) / scale)) - 1);
    while (span < logical_end - from && device_end(from, span) < target_end) {
      ++span;
    }
    return span;
  };
  const int narrowed = std::max(outer_start, start);
  return Span{narrowed, covering_length(narrowed)};
}

}  // namespace scoped_render_clip_detail

// `clip` narrowed to the renderer's current clip, if one is set. A nested clip
// must never WIDEN the outer one: a partial repaint runs the whole scene under
// the dirty rect's clip, and a surface that simply set its own clip drew outside
// that rect -- over pixels that were already right. Opaque fills come out the
// same; anti-aliased text blended over itself does not, so a banner message got
// bolder on every partial frame that ran its paint code without covering it
// (TD-2026-10-15-341 found it). The narrowing is done in device pixels (see
// NarrowAxis). No device pixel in common is a zero-area clip, which SDL enables
// and which draws nothing -- what the caller should get.
inline SDL_Rect NarrowToCurrentClip(SDL_Renderer* renderer, const SDL_Rect& clip) {
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

// Applies `clip`, narrowed to any outer clip, to the renderer for the lifetime of
// the scope, then restores the renderer's PREVIOUS clip state exactly — the
// enabled rect if one was set, or the disabled state if none was.
//
// This exists because the naive "save, set, restore-to-nullptr" pattern silently
// breaks two ways:
//   * SDL_GetRenderClipRect reports success/failure, not whether a clip is active
//     (it fills an empty rect when clipping is disabled), so it cannot be used to
//     decide whether to restore. Use SDL_RenderClipEnabled for that.
//   * SDL_SetRenderClipRect ENABLES a zero-area clip for a {0,0,0,0} rect; only a
//     NULL pointer disables clipping. So restoring a saved-but-empty rect blanks
//     everything drawn afterward, and restoring to nullptr drops an outer clip
//     (e.g. the dirty-region rect a partial repaint relies on) instead of keeping
//     it — letting later draws escape the dirty region.
class ScopedRenderClip {
 public:
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

}  // namespace microide::render
