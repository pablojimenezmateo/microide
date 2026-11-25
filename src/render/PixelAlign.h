#pragma once

#include <cmath>
#include <limits>

namespace microide::render {

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

inline float DeviceAlignedOrigin(float logical, float scale) {
  if (!(scale > 0.0f)) {
    return logical;
  }
  return DeviceExactLogical(std::round(logical * scale), scale);
}

}
