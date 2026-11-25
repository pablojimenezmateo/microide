#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

inline TextGridInteractionLayout BuildEditorInteractionGrid(const render::TextRenderer& text_renderer,
                                                            const editor::TextViewport& viewport,
                                                            const SDL_FRect& rect,
                                                            bool show_line_numbers) {
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, viewport.sticky_band_rows(), show_line_numbers);
  return ComputeTextGridInteractionLayout(
      rect, metrics.text_x, metrics.first_line_y, metrics.line_height, text_renderer.CharWidth(),
      viewport.scroll_line(), viewport.visual_line_count(), viewport.horizontal_scroll(),
      metrics.visible_rows, metrics.visible_columns);
}

}
