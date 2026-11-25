#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace microide::render {

namespace device_pixel_snap_detail {

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

}

inline constexpr float kSnapEdgeNoise = 1.0f / 256.0f;

inline SDL_Rect SnapToDevicePixels(const SDL_FRect& rect, SDL_FPoint scale) {
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

inline float SnapToDevicePixel(float value, float scale) {
  if (!(scale > 0.0f) || !std::isfinite(value)) {
    return value;
  }
  return std::round(value * scale) / scale;
}

inline SDL_Rect SnapToDevicePixels(SDL_Renderer* renderer, const SDL_FRect& rect) {
  return SnapToDevicePixels(rect, device_pixel_snap_detail::DeviceScale(renderer));
}

inline SDL_FRect SnapToDevicePixelsF(SDL_Renderer* renderer, const SDL_FRect& rect) {
  const SDL_Rect snapped = SnapToDevicePixels(renderer, rect);
  return SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                   static_cast<float>(snapped.h)};
}

}
