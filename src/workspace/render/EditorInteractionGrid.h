#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

// The text grid an editor pane resolves a point against, as the frame painted it:
// below the sticky-scroll band when one shows (the band the last frame recorded on
// the viewport). Built with no band, a hover over a function body scrolled under
// its pinned header resolved the line the band's height further down -- the card
// for another symbol -- while a click (which sizes the band itself) landed right.
inline TextGridInteractionLayout BuildEditorInteractionGrid(const render::TextRenderer& text_renderer,
                                                            const editor::TextViewport& viewport,
                                                            const SDL_FRect& rect,
                                                            bool show_line_numbers) {
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, viewport.sticky_band_rows(), show_line_numbers);
  return ComputeTextGridInteractionLayout(
      rect, metrics.text_x, metrics.first_line_y, metrics.line_height, text_renderer.CharWidth(),
      // visual_line_count(), not line_count(): the layout clamps the scroll it is
      // handed against this total, and `scroll_line()` is a visual row -- so under
      // wrap every hover hit test past (line_count - visible_rows) resolved
      // against a scroll position that had been clamped back down.
      viewport.scroll_line(), viewport.visual_line_count(), viewport.horizontal_scroll(),
      metrics.visible_rows, metrics.visible_columns);
}

}  // namespace microide::workspace
