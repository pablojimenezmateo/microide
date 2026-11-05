#pragma once

#include <SDL3/SDL.h>

// Colour-space math primitives shared by the theme engine and any renderer that
// needs WCAG-style contrast or alpha compositing. These are pure functions with
// no theme/state dependencies.
namespace microide::render {

// WCAG relative luminance of an sRGB colour (alpha ignored).
float RelativeLuminance(SDL_Color color);

// WCAG contrast ratio between two colours (always >= 1.0).
float Contrast(SDL_Color c1, SDL_Color c2);

// Linear interpolation between two RGBA colours. `amount` is clamped to [0, 1].
SDL_Color BlendColors(SDL_Color base, SDL_Color tint, float amount);

// Alpha-over composite of `foreground` onto `background`; result is opaque.
SDL_Color CompositeOver(SDL_Color foreground, SDL_Color background);

// `foreground`, moved toward black or white (whichever `background` is not) in
// small steps until its contrast against `background` reaches
// `minimum_contrast`; unchanged when it already does. The best reachable colour
// when no step gets there. Alpha is kept.
SDL_Color EnsureContrast(SDL_Color foreground, SDL_Color background, float minimum_contrast);

}  // namespace microide::render
