#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

class TextRenderer;
class SurfaceTextureCache;
struct PluginDisplayList;

struct DisplayListReplayParams {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
};

void ReplayDisplayList(SDL_Renderer* renderer, TextRenderer& text_renderer,
                       SurfaceTextureCache& texture_cache, const PluginDisplayList& list,
                       const DisplayListReplayParams& params);

}
