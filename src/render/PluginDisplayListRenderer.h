#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

class TextRenderer;
class SurfaceTextureCache;
struct PluginDisplayList;

// Where to draw the content and how content-local coordinates map to the screen.
// `origin_x/y` is the screen position of content coordinate (0, 0) — callers fold
// the surface origin and any host-owned scroll into it. `clip` bounds everything
// the replay draws (panel rect or inset gap rect); ops never paint outside it.
struct DisplayListReplayParams {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
};

// The single host-owned routine that paints a plugin display list, shared by the
// preview panel (E0) and inline insets (E1). Reuses TextRenderer + fill/line
// primitives + the texture cache for Image ops. Plugins never touch SDL; this is
// the only place their op buffer turns into draw calls. The list must have passed
// ValidateDisplayList already, so replay does no per-op bounds checks.
void ReplayDisplayList(SDL_Renderer* renderer, TextRenderer& text_renderer,
                       SurfaceTextureCache& texture_cache, const PluginDisplayList& list,
                       const DisplayListReplayParams& params);

}  // namespace microide::render
