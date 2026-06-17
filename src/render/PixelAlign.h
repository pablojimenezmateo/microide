#pragma once

#include <cmath>

namespace microide::render {

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
  return std::round(logical * scale) / scale;
}

}  // namespace microide::render
