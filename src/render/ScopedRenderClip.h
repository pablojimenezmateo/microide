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

// One axis of the narrowing. SDL maps a clip's [start, start + length) to the
// device as [floor(start*s), floor(start*s) + ceil(length*s)) -- SDL 3.2's
// UpdatePixelClipRect -- so at a fractional scale the logical intersection is
// not the device intersection, and not every device span is a logical one. This
// returns a logical span whose device span covers the device intersection of
// `outer` and `requested` -- a row left out keeps whatever an earlier frame drew
// there (CI qa, menu-clicks: the sidebar's focus ring under a closing menu) --
// and never leaves the outer one: a row painted past it lands under whatever
// the partial frame does not repaint (a submenu, a picker's backdrop: CI qa and
// the QA monkey's seed 3). Where no span from the intersection's own start ends
// inside the outer clip, it starts a little earlier, still inside it; the outer
// span itself always fits. What is left: the end may pass the requested edge
// by a device row inside the outer clip (TD-2026-10-22-376).

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
  // The shortest span from `from` whose device end reaches target_end, when that
  // end stays inside the outer clip; -1 when it cannot.
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
// `rect` grown outward to whole device pixels at the renderer's scale. Every
// clip a frame sets goes through this -- the partial frame's damage clips
// (Application) and each surface's own (ScopedRenderClip) -- so at a fractional
// scale a full frame and a partial one clip a surface to the same device pixels;
// with unaligned edges SDL's separate rounding of start and length made them
// differ by a row at a surface's edge (TD-2026-10-22-376). A surface may then
// paint up to k-1 logical pixels past its edge, the same in both frames.
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
  // A pane's float rect, rounded out to whole pixels the way every pane clip is.
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

}  // namespace microide::render
