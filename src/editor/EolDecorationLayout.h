#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "render/TextRenderer.h"

namespace microide::editor {

struct EolDecorationSegment {
  enum class Kind : std::uint8_t { CodeLens, InlineText };
  Kind kind = Kind::InlineText;
  std::uint32_t index = 0;
  SDL_FRect rect{};
};

void BuildEolDecorationSegments(const render::TextRenderer& text_renderer,
                                std::span<const InlineTextDecoration> inline_texts,
                                std::span<const CodeLensDecoration> code_lenses,
                                float anchor_x,
                                float y,
                                float line_height,
                                float right_limit,
                                std::vector<EolDecorationSegment>& out);

}
