#include "render/DebugTextBackend.h"
#include "render/SurfacePrimitives.h"

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

  SetDrawColor(renderer, color);
  const std::string owned_text(text);
  SDL_RenderDebugText(renderer, x, y, owned_text.c_str());
}

}  // namespace microide::render
