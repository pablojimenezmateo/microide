#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "editor/EolDecorationLayout.h"
#include "editor/InlayHintColumns.h"
#include "editor/PluginDecorationStore.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

// The activatable code lenses painted at the end of `line_index`, each with its
// rect, laid out exactly as the renderer lays them out: anchored past the line's
// last glyph and any inlay hints (none on a soft-wrapped line, as the render path
// omits them there). The click path hit-tests these rects and the control query
// lists them, so a lens is clicked where it is drawn. `emit(lens, rect)`.
template <typename Emit>
void ForEachLineCodeLens(const render::TextRenderer& text_renderer,
                         const editor::TextViewport& viewport,
                         const TextGridInteractionLayout& interaction,
                         const editor::FileDecorations& decorations,
                         std::size_t line_index,
                         std::vector<editor::EolDecorationSegment>& segments_scratch,
                         Emit&& emit) {
  if (line_index >= viewport.lines().size()) {
    return;
  }
  const std::span<const editor::CodeLensDecoration> code_lenses =
      decorations.CodeLensesForLine(static_cast<std::uint32_t>(line_index));
  if (code_lenses.empty()) {
    return;
  }
  const std::span<const editor::InlineTextDecoration> inline_texts =
      decorations.InlineTextsForLine(static_cast<std::uint32_t>(line_index));
  const float line_y = interaction.first_line_y +
                       static_cast<float>(line_index - interaction.scroll_line) * interaction.line_height;
  const std::size_t inlay_total_cells =
      viewport.soft_wrap()
          ? 0
          : editor::InlayLineTotalCells(inline_texts, text_renderer, text_renderer.CharWidth());
  const float anchor_x =
      interaction.text_x +
      static_cast<float>(viewport.VisibleLineLayout(line_index).visual_columns + inlay_total_cells) *
          text_renderer.CharWidth();
  const float right_limit = interaction.rect.x + interaction.rect.w - 12.0f;
  segments_scratch.clear();
  editor::BuildEolDecorationSegments(text_renderer, inline_texts, code_lenses, anchor_x, line_y,
                                     interaction.line_height, right_limit, segments_scratch);
  for (const editor::EolDecorationSegment& segment : segments_scratch) {
    if (segment.kind == editor::EolDecorationSegment::Kind::CodeLens &&
        code_lenses[segment.index].activatable()) {
      emit(code_lenses[segment.index], segment.rect);
    }
  }
}

}  // namespace microide::workspace
