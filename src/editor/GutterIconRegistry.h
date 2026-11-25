#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

#include "editor/PluginDecorationStore.h"

namespace microide::editor {

class GutterIconRegistry {
 public:
  static std::optional<GutterIconShape> ResolveShape(std::string_view name);

  static SDL_FRect MarkerRect(float gutter_x, float y, float gutter_width, float line_height);

  static void Draw(SDL_Renderer* renderer, GutterIconShape shape, SDL_Color color, float gutter_x,
                   float y, float gutter_width, float line_height);
};

}
