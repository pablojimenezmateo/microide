#pragma once

#include <cmath>
#include <limits>

namespace microide::render {

// The logical coordinate or length a renderer maps onto exactly `device_px`
// physical pixels. SDL's software renderer turns a texture destination into
// device pixels as (int)(value * scale) in float, for the origin and the extent
// separately, and device_px / scale * scale often lands one ulp short of
// device_px -- which truncates to device_px - 1. For an extent that turns a 1:1
// glyph copy into a NEAREST-scaled one (a column dropped, and a blit that samples
// differently wherever a clip edge cuts it); for an origin it moves the glyph a
// pixel. So step the quotient away from zero until the product reaches device_px.
// The adjustment is a few ulps: no renderer that rounds instead of truncating can
// tell the difference.
inline float DeviceExactLogical(float device_px, float scale) {
  if (!(scale > 0.0f) || !std::isfinite(scale)) {
    return device_px;
  }
  const float away = device_px < 0.0f ? -std::numeric_limits<float>::infinity()
                                      : std::numeric_limits<float>::infinity();
  float logical = device_px / scale;
  while (std::fabs(logical * scale) < std::fabs(device_px)) {
    logical = std::nextafter(logical, away);
  }
  return logical;
}

// Snap a logical origin so it maps onto an integer physical pixel under the
// active presentation (logical->physical) scale. Glyph textures are rasterized
// at physical resolution and sampled with SDL_SCALEMODE_NEAREST, so they stay
// 1:1 with the device grid only when their origin lands on a whole physical
// pixel. Centered/offset surfaces (Help/About, Settings) otherwise place text
// at fractional physical positions, which smears glyph stems under NEAREST.
// A no-op at integer scales: round(logical * scale) / scale round-trips exactly.
inline float DeviceAlignedOrigin(float logical, float scale) {
  if (!(scale > 0.0f)) {
    return logical;
  }
  return DeviceExactLogical(std::round(logical * scale), scale);
}

}  // namespace microide::render
