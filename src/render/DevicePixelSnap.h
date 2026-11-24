#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

// One rule for turning a logical rect into whole device pixels, shared by every
// clip (ScopedRenderClip, the partial frame's damage clips), every damage request
// and every derived-damage unit (openspec clip-discipline D2). A clip and a fill or
// a damage rect computed separately disagreed by a device row at fractional scales
// -- paint one pixel past the edge its damage covered (the `clip-edge` stale-paint
// class) -- and the same rect snapped twice the same way cannot.
namespace microide::render {

namespace device_pixel_snap_detail {

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

// The smallest k for which k logical pixels are a whole number of device pixels
// at `scale` (1 at 1x and 2x, 2 at 1.5x, 4 at 1.25x and 1.75x, 10 at 1.1x and
// 1.3x, 20 at 1.65x; 1 when nothing up to kMaxDevicePixelAlignment fits). A rect
// whose edges are multiples of k maps to the device with no rounding, so two such
// rects intersect exactly (TD-2026-10-22-376).
//
// The search stopped at 8, and `ui.scale` takes any value (the presets are
// quarters; the setting is a float, and the display's own scale multiplies it):
// at 1.1x nothing up to 8 fits, the clips went unaligned, and a surface's clip
// narrowed under a partial frame's reached a device column past where its
// full-frame clip stops -- the sidebar painted a pixel of the editor in partial
// frames only (soak d213837376ca). 20 covers every scale in steps of 0.05; a rect
// then grows by up to 19 logical pixels a side.
inline constexpr int kMaxDevicePixelAlignment = 20;
inline int DevicePixelAlignment(float scale) {
  for (int k = 1; k <= kMaxDevicePixelAlignment; ++k) {
    const float device = static_cast<float>(k) * scale;
    if (std::fabs(device - std::round(device)) < 1e-3f) {
      return k;
    }
  }
  return 1;
}

struct Span {
  int start = 0;
  int length = 0;
};

// [start, start + length) grown outward to multiples of k.
inline Span SnapOutward(int start, int length, int k) {
  if (k <= 1 || length <= 0) {
    return Span{start, length};
  }
  const auto floor_to = [k](int value) { return value >= 0 ? value / k * k : -((-value + k - 1) / k * k); };
  const int snapped_start = floor_to(start);
  const int end = start + length;
  const int snapped_end = floor_to(end) == end ? end : floor_to(end) + k;
  return Span{snapped_start, snapped_end - snapped_start};
}

}  // namespace device_pixel_snap_detail

// The integral logical rect covering `rect` whose edges are whole device pixels at
// `scale`: floor of the left and top, ceil of the right and bottom, then grown
// outward to the scale's alignment k. Idempotent. An empty, inverted or NaN rect is
// {0,0,0,0} -- never one alignment unit, which would damage and clip k x k pixels
// for a rect of none.
// How far from a whole logical pixel an edge may sit and still snap as that pixel.
// Layout arithmetic leaves float noise on edges meant to coincide: a terminal
// scrollbar's thumb laid out to end where its track ends summed to
// 882.857178 + 53.1428566 = 936.0000x (soak 0ab3769c6866, from its rr trace),
// which ceiled to 937 and then aligned out a whole step -- 4 px at 1.25x -- past
// the track it shares that edge with. Its element reported it drawing outside,
// and the clips and damage of two rects meeting there disagreed.
inline constexpr float kSnapEdgeNoise = 1.0f / 256.0f;

inline SDL_Rect SnapToDevicePixels(const SDL_FRect& rect, SDL_FPoint scale) {
  // `!(w > 0)` is also true for NaN.
  if (!(rect.w > 0.0f) || !(rect.h > 0.0f) || std::isnan(rect.x) || std::isnan(rect.y)) {
    return SDL_Rect{0, 0, 0, 0};
  }
  const int left = static_cast<int>(std::floor(rect.x + kSnapEdgeNoise));
  const int top = static_cast<int>(std::floor(rect.y + kSnapEdgeNoise));
  const int right = std::max(left + 1, static_cast<int>(std::ceil(rect.x + rect.w - kSnapEdgeNoise)));
  const int bottom = std::max(top + 1, static_cast<int>(std::ceil(rect.y + rect.h - kSnapEdgeNoise)));
  const auto x = device_pixel_snap_detail::SnapOutward(
      left, right - left, device_pixel_snap_detail::DevicePixelAlignment(scale.x));
  const auto y = device_pixel_snap_detail::SnapOutward(
      top, bottom - top, device_pixel_snap_detail::DevicePixelAlignment(scale.y));
  return SDL_Rect{x.start, y.start, x.length, y.length};
}

// A logical coordinate moved onto the device pixel nearest it at `scale`. For a
// thin fill placed at a MEASURED position -- a caret after a run of glyph
// advances, each a device width over the scale -- which carries float noise. A
// hair under a whole logical pixel, the fill starts a device column to the left
// of that pixel, while SnapToDevicePixels takes the same noise for the whole
// pixel (kSnapEdgeNoise) and starts the damage rect meant to cover it AT the
// pixel: at 3x a caret at 175.9997 painted device column 527 under a clip that
// began at 528, and every blink left that column lit (soak c47614ab72b7). Placed
// on a device pixel, the fill and its damage agree.
inline float SnapToDevicePixel(float value, float scale) {
  if (!(scale > 0.0f) || !std::isfinite(value)) {
    return value;
  }
  return std::round(value * scale) / scale;
}

// The same at the renderer's current device scale.
inline SDL_Rect SnapToDevicePixels(SDL_Renderer* renderer, const SDL_FRect& rect) {
  return SnapToDevicePixels(rect, device_pixel_snap_detail::DeviceScale(renderer));
}

// The same snapped rect as a float rect: a derived-damage unit's rect, snapped
// exactly as its PaintElement clips (clip-discipline D2).
inline SDL_FRect SnapToDevicePixelsF(SDL_Renderer* renderer, const SDL_FRect& rect) {
  const SDL_Rect snapped = SnapToDevicePixels(renderer, rect);
  return SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                   static_cast<float>(snapped.h)};
}

}  // namespace microide::render
