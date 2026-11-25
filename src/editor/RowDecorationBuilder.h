#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsStore.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::editor {

void PushWhitespaceMarker(std::vector<DecoratedTextFill>& fills,
                          bool is_tab,
                          float cell_x,
                          float char_width,
                          float cell_span_width,
                          float y,
                          float line_height,
                          SDL_Color color);

class CellShiftRef {
 public:
  template <typename Fn>
  CellShiftRef(const Fn& shift)  // NOLINT(google-explicit-constructor)
      : object_(&shift), invoke_([](const void* object, std::size_t cell) {
          return (*static_cast<const Fn*>(object))(cell);
        }) {}

  float operator()(std::size_t absolute_visual_column) const {
    return invoke_(object_, absolute_visual_column);
  }

 private:
  const void* object_ = nullptr;
  float (*invoke_)(const void*, std::size_t) = nullptr;
};

struct WhitespaceWalkCursor {
  std::size_t line_key = 0;
  std::size_t byte = 0;
  std::size_t visual_col = 0;
  bool valid = false;
};

struct WhitespaceRowResume {
  WhitespaceWalkCursor* cursor = nullptr;
  std::size_t line_key = 0;
  std::size_t plain_prefix_end = 0;
};

struct WhitespaceWalkStart {
  std::size_t byte = 0;
  std::size_t visual_col = 0;
};

WhitespaceWalkStart ResolveWhitespaceWalkStart(std::string_view text,
                                               std::size_t row_visual_start,
                                               WhitespaceRowResume resume);

void RecordWhitespaceWalkStop(WhitespaceRowResume resume,
                              std::size_t byte,
                              std::size_t visual_col);

void InvalidateWhitespaceWalkCursor(WhitespaceRowResume resume);

std::size_t AppendWhitespaceMarkers(std::vector<DecoratedTextFill>& fills,
                                    std::string_view text,
                                    std::size_t tab_size,
                                    std::size_t row_visual_start,
                                    std::size_t row_visual_end,
                                    float text_x,
                                    float char_width,
                                    float y,
                                    float line_height,
                                    SDL_Color color,
                                    CellShiftRef shift_px,
                                    WhitespaceRowResume resume = {});

std::size_t AppendWhitespaceMarkers(std::vector<DecoratedTextFill>& fills,
                                    std::string_view text,
                                    std::size_t tab_size,
                                    std::size_t row_visual_start,
                                    std::size_t row_visual_end,
                                    float text_x,
                                    float char_width,
                                    float y,
                                    float line_height,
                                    SDL_Color color,
                                    WhitespaceRowResume resume = {});

struct RowFillSpan {
  std::size_t start_column = 0;
  std::size_t end_column = 0;
  SDL_Color color{};
  enum class Geometry {
    kRange,
    kSingleCell,
  } geometry = Geometry::kRange;
};

struct RowDecorationInput {
  float text_x = 0.0f;
  float y = 0.0f;
  float char_width = 0.0f;
  float line_height = 0.0f;

  std::size_t row_visual_start = 0;
  std::size_t row_visual_end = 0;

  std::string_view text;
  std::size_t line_length = 0;
  const std::vector<SyntaxTokenKind>* tokens = nullptr;
  SDL_Color plain_color{};

  const LayoutLine* layout = nullptr;
  const TextLayout::LineVisualColumnMap* visual_map = nullptr;

  bool has_background_fill = false;
  DecoratedTextFill background_fill{};
  bool has_edge_stripe = false;
  DecoratedTextFill edge_stripe_fill{};

  std::span<const RowFillSpan> column_fills;

  std::span<const DecoratedTextFill> prepositioned_fills;

  std::span<const compare::CompareTextSpan> changed_spans;
  SDL_Color changed_span_fill{};

  std::span<const PublishedDiagnostic> diagnostics;
  std::size_t diagnostic_line_index = 0;
  std::size_t diagnostic_horizontal_scroll = 0;
  std::size_t diagnostic_visible_columns = 0;
  std::size_t tab_size = 0;

  std::span<const TextStyleDecoration> text_styles;

  InlayRowDisplacement inlay;
  std::span<const InlineTextDecoration> inlay_inline_texts;
  SDL_Color inlay_color{};

  const render::TextRenderer* text_renderer = nullptr;
  const render::Theme* theme = nullptr;
};

void BuildDecoratedRow(DecoratedTextRow& row, const RowDecorationInput& in);

}
