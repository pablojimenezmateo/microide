#include "render/DebugTextBackend.h"

#include <string>

namespace microide::render {

void DebugTextBackend::DrawString(SDL_Renderer* renderer,
                                  float x,
                                  float y,
                                  SDL_Color color,
                                  std::string_view text) {
  if (renderer == nullptr || text.empty()) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const std::string owned_text(text);
  SDL_RenderDebugText(renderer, x, y, owned_text.c_str());
}

}  // namespace microide::render
