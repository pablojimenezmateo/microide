#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

float RelativeLuminance(SDL_Color color);

float Contrast(SDL_Color c1, SDL_Color c2);

SDL_Color BlendColors(SDL_Color base, SDL_Color tint, float amount);

SDL_Color CompositeOver(SDL_Color foreground, SDL_Color background);

SDL_Color EnsureContrast(SDL_Color foreground, SDL_Color background, float minimum_contrast);

}
