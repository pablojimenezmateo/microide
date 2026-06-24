#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "render/TextRenderer.h"

namespace microide::editor {

// One laid-out end-of-line decoration segment: a pixel rect plus a back-reference
// to the source decoration. Render and hit-test resolve the text/color/command by
// indexing the originating span, so this struct stays a pure geometry record and
// the two passes can never disagree on placement.
struct EolDecorationSegment {
  enum class Kind : std::uint8_t { CodeLens, InlineText };
  Kind kind = Kind::InlineText;
  std::uint32_t index = 0;  // index into the matching code_lenses / inline_texts span
  SDL_FRect rect{};
};

// Lay out a logical line's end-of-line decorations left-to-right starting past the
// line's last glyph. Code lenses (clickable command affordances) come first, then
// informational inline texts; each is separated by a fixed gap and the run is
// clipped at `right_limit` (a segment that would overflow is dropped, as are all
// after it). Only inline texts anchored at end-of-line are included; mid-line
// virtual text is deferred (Phase B v1 is EOL-only).
//
// `anchor_x` is the x of the line's last glyph edge (text_x + visual_columns *
// char_width); the first segment begins one gap past it. Pure geometry: the text
// renderer is used only for width measurement, never to draw, so this is unit
// testable and allocation-free apart from growing the caller-reused `out`.
void BuildEolDecorationSegments(const render::TextRenderer& text_renderer,
                                std::span<const InlineTextDecoration> inline_texts,
                                std::span<const CodeLensDecoration> code_lenses,
                                float anchor_x,
                                float y,
                                float line_height,
                                float right_limit,
                                std::vector<EolDecorationSegment>& out);

}  // namespace microide::editor
