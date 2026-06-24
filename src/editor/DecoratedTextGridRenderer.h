#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::editor {

struct VisibleTextWindow {
  std::string_view text;
  std::size_t byte_offset = 0;
};

struct DecoratedTextFill {
  SDL_FRect rect{};
  SDL_Color color{};
};

struct DecoratedTextRun {
  float x = 0.0f;
  float y = 0.0f;
  SDL_Color color{};
  std::string_view text;
};

struct DecoratedUnderline {
  SDL_FRect rect{};
  SDL_Color color{};
};

struct DecoratedTextRow {
  std::vector<DecoratedTextFill> fills;
  std::vector<DecoratedTextRun> runs;
  std::vector<DecoratedUnderline> underlines;
};

VisibleTextWindow SliceVisibleColumns(std::string_view text,
                                      std::size_t start_column,
                                      std::size_t visible_columns);

SDL_Color SyntaxTokenColor(const render::Theme& theme,
                           SyntaxTokenKind kind,
                           SDL_Color fallback);

void AppendVisibleSyntaxTextRuns(DecoratedTextRow& row,
                                 const render::TextRenderer& text_renderer,
                                 const render::Theme& theme,
                                 float x,
                                 float y,
                                 std::string_view text,
                                 std::size_t horizontal_scroll,
                                 std::size_t visible_columns,
                                 SDL_Color plain_color,
                                 const std::vector<SyntaxTokenKind>& full_tokens,
                                 std::span<const TextStyleDecoration> foreground_overrides = {});

void AppendLayoutSyntaxTextRuns(DecoratedTextRow& row,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                float x,
                                float y,
                                const LayoutLine& layout,
                                SDL_Color plain_color,
                                const std::vector<SyntaxTokenKind>& full_tokens,
                                std::span<const TextStyleDecoration> foreground_overrides = {});

class DecoratedTextGridRenderer {
 public:
  void RenderRow(SDL_Renderer* renderer,
                 const render::TextRenderer& text_renderer,
                 const DecoratedTextRow& row) const;

 private:
  // Scratch buffer used to coalesce same-color fill runs into a single
  // SDL_RenderFillRects batch. Reused across rows; capacity persists across
  // frames so per-row batching does not allocate. Mutable so RenderRow stays
  // logically const.
  mutable std::vector<SDL_FRect> fill_batch_scratch_;
};

}  // namespace microide::editor
