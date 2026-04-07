#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::editor {

struct EditorViewMetrics {
  float gutter_width = 56.0f;
  float text_x = 0.0f;
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  std::size_t visible_rows = 1;
  std::size_t visible_columns = 8;
};

class EditorViewRenderer {
 public:
  static EditorViewMetrics ComputeMetrics(const render::TextRenderer& text_renderer,
                                          const TextViewport& viewport,
                                          const SDL_FRect& rect);

  void Render(SDL_Renderer* renderer,
              const render::TextRenderer& text_renderer,
              const render::Theme& theme,
              TextViewport& viewport,
              const SDL_FRect& rect,
              bool draw_caret = true,
              std::string_view search_query = {},
              const std::optional<SelectionRange>& active_search_match = std::nullopt) const;
};

}  // namespace microide::editor
