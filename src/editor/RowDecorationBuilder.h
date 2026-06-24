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

// Append intra-line word/token diff underlines for the visible window of `text`
// to `row`. Clips the changed byte spans to the horizontally-scrolled visible
// columns and emits a dimmed underline (0.55 alpha) under each changed run.
// Shared by the compare side panes and merge; keep the alpha dim in sync with
// the renderer. Moved here from workspace/CompareMergeRender so the unified
// decorated-row builder can own intra-line underline assembly.
void AppendChangedSpanUnderlines(DecoratedTextRow& row,
                                 const render::TextRenderer& text_renderer,
                                 float text_x,
                                 float y,
                                 float line_height,
                                 std::string_view text,
                                 std::size_t horizontal_scroll,
                                 std::size_t visible_columns,
                                 std::span<const compare::CompareTextSpan> changed_spans,
                                 SDL_Color underline_color);

// A row background/match/selection/bracket highlight expressed in source byte
// columns. The builder resolves source -> visual columns (via the row layout or
// the visual-column map) and emits the pixel fill. Express fills in submission
// order; the builder never reorders them.
struct RowFillSpan {
  std::size_t start_column = 0;
  std::size_t end_column = 0;  // exclusive (ignored for kSingleCell)
  SDL_Color color{};
  enum class Geometry {
    kRange,       // [start_column, end_column) clipped to the visible window
    kSingleCell,  // one cell at start_column (bracket match)
  } geometry = Geometry::kRange;
};

// POD inputs for BuildDecoratedRow. Every variable-length input is a span or
// pointer into caller-owned storage; the builder materializes no strings and
// keeps all hot-path work allocation-free (the row's vectors are caller-reused
// scratch). Surface-specific decorations (diff tint, edge stripe, changed
// spans, selection, diagnostics) are data here, not separate code paths.
struct RowDecorationInput {
  // Glyph origin + cell metrics (shared by fills, runs, underlines).
  float text_x = 0.0f;
  float y = 0.0f;
  float char_width = 0.0f;
  float line_height = 0.0f;

  // Visible window in visual columns, used to resolve/clip column fills and to
  // derive the visible-syntax and changed-span windows.
  std::size_t row_visual_start = 0;
  std::size_t row_visual_end = 0;

  // Canonical line text. Drives the visible-syntax / changed-span string_view
  // and the diagnostic underline pass (which needs a std::string). May be null
  // for an empty row.
  const std::string* text = nullptr;
  const std::vector<SyntaxTokenKind>* tokens = nullptr;
  SDL_Color plain_color{};

  // Syntax-run path selector. Non-null `layout` => cached cell-grid path
  // (editor / converged compare-merge). Null => visible-window re-walk path,
  // with `visual_map` (optional) resolving column fills.
  const LayoutLine* layout = nullptr;
  const TextLayout::LineVisualColumnMap* visual_map = nullptr;

  // Optional fully-positioned background band + diff edge stripe (caller owns
  // the exact rects, which differ per surface).
  bool has_background_fill = false;
  DecoratedTextFill background_fill{};
  bool has_edge_stripe = false;
  DecoratedTextFill edge_stripe_fill{};

  // Source-column fills resolved by the builder (search / occurrence /
  // selection / bracket). Appended in order, after the background + stripe.
  std::span<const RowFillSpan> column_fills;

  // Pre-positioned fills appended verbatim after `column_fills` (whitespace
  // glyphs, indent guides — their sub-cell geometry is resolved by the caller).
  std::span<const DecoratedTextFill> prepositioned_fills;

  // Intra-line changed-span underlines (compare / merge). Empty => skipped.
  std::span<const compare::CompareTextSpan> changed_spans;
  SDL_Color changed_span_color{};

  // Diagnostic underlines. Empty => skipped. The diagnostic window can differ
  // from `row_visual_start/end` (the editor passes the wrapped-row visual span),
  // so it is carried explicitly.
  std::span<const PublishedDiagnostic> diagnostics;
  std::size_t diagnostic_line_index = 0;
  std::size_t diagnostic_horizontal_scroll = 0;
  std::size_t diagnostic_visible_columns = 0;
  std::size_t tab_size = 0;

  // Plugin-published inline text-style decorations for this line. Drives
  // background fills, a foreground recolor overlay on the syntax runs, and
  // underline/strike. Empty => skipped (zero overhead for the common case).
  std::span<const TextStyleDecoration> text_styles;

  const render::TextRenderer* text_renderer = nullptr;
  const render::Theme* theme = nullptr;
};

// Clear `row` and assemble its fills / syntax runs / underlines from `in`, in
// the canonical submission order shared by editor, compare, and merge:
//   background -> edge stripe -> column fills -> pre-positioned fills ->
//   syntax runs -> changed-span underlines -> diagnostic underlines.
// The caller renders the row afterward (DecoratedTextGridRenderer::RenderRow);
// the builder itself touches no SDL_Renderer so it stays unit-testable.
void BuildDecoratedRow(DecoratedTextRow& row, const RowDecorationInput& in);

}  // namespace microide::editor
