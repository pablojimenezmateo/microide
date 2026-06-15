#include "render/ColorMath.h"

#include <algorithm>
#include <cmath>

namespace microide::render {

float RelativeLuminance(SDL_Color color) {
  auto srgb_to_linear = [](Uint8 value) -> float {
    float v = static_cast<float>(value) / 255.0f;
    if (v <= 0.04045f) {
      return v / 12.92f;
    }
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
  };

  float r = srgb_to_linear(color.r);
  float g = srgb_to_linear(color.g);
  float b = srgb_to_linear(color.b);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float Contrast(SDL_Color c1, SDL_Color c2) {
  float l1 = RelativeLuminance(c1);
  float l2 = RelativeLuminance(c2);
  if (l1 < l2) {
    std::swap(l1, l2);
  }
  return (l1 + 0.05f) / (l2 + 0.05f);
}

SDL_Color BlendColors(SDL_Color base, SDL_Color tint, float amount) {
  const float clamped = std::clamp(amount, 0.0f, 1.0f);
  const auto blend = [&](Uint8 a, Uint8 b) -> Uint8 {
    return static_cast<Uint8>(std::lround(static_cast<float>(a) * (1.0f - clamped) +
                                          static_cast<float>(b) * clamped));
  };
  return SDL_Color{
      blend(base.r, tint.r),
      blend(base.g, tint.g),
      blend(base.b, tint.b),
      blend(base.a, tint.a),
  };
}

SDL_Color CompositeOver(SDL_Color foreground, SDL_Color background) {
  const float alpha = static_cast<float>(foreground.a) / 255.0f;
  const auto over = [&](Uint8 fg, Uint8 bg) -> Uint8 {
    return static_cast<Uint8>(
        std::clamp(std::lround(static_cast<float>(fg) * alpha +
                               static_cast<float>(bg) * (1.0f - alpha)),
                   0l, 255l));
  };
  return SDL_Color{
      over(foreground.r, background.r),
      over(foreground.g, background.g),
      over(foreground.b, background.b),
      0xff,
  };
}

}  // namespace microide::render
