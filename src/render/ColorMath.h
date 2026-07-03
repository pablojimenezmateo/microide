#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

// Colour-space math primitives shared by the theme engine and any renderer that
// needs WCAG-style contrast or alpha compositing. These are pure functions with
// no theme/state dependencies.
namespace microide::render {

// Construct an SDL_Color, opaque by default. constexpr so palette tables can be
// built at compile time.
constexpr SDL_Color MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff) {
  return SDL_Color{r, g, b, a};
}

// Exact RGBA equality.
constexpr bool ColorsEqual(SDL_Color a, SDL_Color b) noexcept {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Pack RGBA into one 32-bit word (r in the high byte, a in the low byte), for
// hashing a colour as a single value.
constexpr std::uint32_t PackColorRgba(SDL_Color color) noexcept {
  return (static_cast<std::uint32_t>(color.r) << 24) |
         (static_cast<std::uint32_t>(color.g) << 16) |
         (static_cast<std::uint32_t>(color.b) << 8) |
         static_cast<std::uint32_t>(color.a);
}

// WCAG relative luminance of an sRGB colour (alpha ignored).
float RelativeLuminance(SDL_Color color);

// WCAG contrast ratio between two colours (always >= 1.0).
float Contrast(SDL_Color c1, SDL_Color c2);

// Linear interpolation between two RGBA colours. `amount` is clamped to [0, 1].
SDL_Color BlendColors(SDL_Color base, SDL_Color tint, float amount);

// Alpha-over composite of `foreground` onto `background`; result is opaque.
SDL_Color CompositeOver(SDL_Color foreground, SDL_Color background);

}  // namespace microide::render
