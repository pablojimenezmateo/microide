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

// Append the render-whitespace marker for one cell: a 2x2 dot centered in the
// cell for a space, or a thin horizontal bar spanning the tab's cells. Lives here
// so the editor pane and the diff panes cannot draw the same marker differently.
void PushWhitespaceMarker(std::vector<DecoratedTextFill>& fills,
                          bool is_tab,
                          float cell_x,
                          float char_width,
                          float cell_span_width,
                          float y,
                          float line_height,
                          SDL_Color color);

// A per-cell horizontal displacement, in pixels, for a marker at an absolute
// visual column. The editor pane's rows shift right past mid-line inlay hints;
// every other surface has none.
//
// Non-owning and type-erased rather than a std::function: the editor's shift
// captures three values, which exceeds libstdc++'s small-object buffer, so a
// std::function here would allocate once per painted row.
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

// Resume state for a whitespace walk driven row by row over the SAME logical
// line, which is what soft wrap makes of every long line: one line owns many
// consecutive visual rows, and each row needs the byte offset its visual start
// sits at.
//
// Deriving that per row is quadratic in the rows of one line. The obvious form
// -- re-walking the text from byte 0 -- was fixed in TD-2026-08-12-187 by
// probing the prefix instead, and the probe has the SAME shape: it reads
// [0, row_visual_start) on every row, so scrolling deep into a wrapped megabyte
// line re-reads ~a megabyte per visible row per frame. Rows arrive in ascending
// order, so the previous row's stopping point IS this row's start, and neither
// read is needed at all (TD-2026-08-14-218).
//
// A cursor is valid for one pass over one line's rows on one frame; hold it as a
// local of the row loop, never across frames -- the text it indexes into may
// have been edited in between.
struct WhitespaceWalkCursor {
  std::size_t line_key = 0;
  std::size_t byte = 0;
  std::size_t visual_col = 0;
  bool valid = false;
};

// The cursor plus which line the caller is about to walk. `line_key` is any
// value stable for one logical line and different between lines; a line index is
// the natural choice. A default-constructed value (null cursor) disables the
// carry and restores the probe-every-row behaviour.
struct WhitespaceRowResume {
  WhitespaceWalkCursor* cursor = nullptr;
  std::size_t line_key = 0;
  // How far into the line the caller already knows every byte is plain
  // single-cell ASCII -- i.e. where byte offset stops being visual column. A row
  // starting at or below this needs no deriving at all. Understating it is safe:
  // it only falls back to the probe. Callers with a viewport get it memoized from
  // `TextViewport::PlainAsciiPrefixEnd`, which matters because the carry alone
  // still leaves one probe per line per frame -- and on a wrapped megabyte line
  // scrolled deep, one probe is ~a megabyte of reading.
  std::size_t plain_prefix_end = 0;
};

// Where a row's whitespace walk should start, in bytes and visual columns.
struct WhitespaceWalkStart {
  std::size_t byte = 0;
  std::size_t visual_col = 0;
};

// Resolve a row's walk start: carry from `resume` when it describes this line at
// or before this row, otherwise probe the prefix (and fall back to byte 0 when
// the prefix is not plain single-cell ASCII). Bumps
// `editor.whitespace_marker_prefix_bytes_scanned` / `..._rows_carried`, which is
// what makes "the row carried" a measurement rather than a comment.
WhitespaceWalkStart ResolveWhitespaceWalkStart(std::string_view text,
                                               std::size_t row_visual_start,
                                               WhitespaceRowResume resume);

// Hand the next row of the same line the cell this row stopped on.
void RecordWhitespaceWalkStop(WhitespaceRowResume resume,
                              std::size_t byte,
                              std::size_t visual_col);

// The line ran out before the row did, so there is no next row to carry to.
void InvalidateWhitespaceWalkCursor(WhitespaceRowResume resume);

// Append render-whitespace markers for the visible window [row_visual_start,
// row_visual_end) of `text`, walking one visual cell per codepoint so tabs
// expand and a multi-byte glyph does not shift every later marker right.
// Returns the number of BYTES the walk visited, which the editor reports as a
// performance counter (the walk resumes at the row rather than restarting at
// byte 0, and "resumed" should be a measurement rather than a comment).
//
// This is the single whitespace walk for every surface: the editor pane passes
// its inlay-hint displacement through `shift_px`, the compare panes pass nothing
// (TD-2026-08-14-210). The editor's view-model path is deliberately NOT this —
// it is a precomputed glyph-run table, not a walk, and is the fast path by
// design.
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

// The same, with no displacement.
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

  // Canonical line text, as a view. A `const std::string*` here forced every
  // caller to hand over an owned string, and the editor's per-row path got one
  // from TextBuffer's `operator[]` -- which materializes a heap copy of the line
  // and keeps it until the next mutation, so painting a large file left a second
  // copy of it resident. Everything downstream only ever read bytes.
  // The line's bytes. On the editor's row path (`layout` set) the ONLY consumer
  // is diagnostic underlining, so that path leaves this EMPTY unless the row
  // actually carries a diagnostic: reading the line to render a row copies any
  // piece-tree line that spans pieces, which on a file with no line breaks in it
  // is megabytes per frame (TD-2026-08-05-133). Anything that needs the line's
  // extent rather than its bytes must read `line_length`, which is always set.
  std::string_view text;
  // Byte length of the whole line, even when `text` is empty. Whole-line plugin
  // decorations end here.
  std::size_t line_length = 0;
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

  // Mid-line inlay hints for this row. `inlay` (row-local, built by
  // BuildInlayRowSpans into caller scratch) shifts every column->x on the row so
  // real glyphs move right past the hints; `inlay_inline_texts` is the line's
  // decoration slice the hint glyphs are drawn from. Empty inlay => identity, so a
  // row without hints (the common case) pays only an emptiness branch.
  InlayRowDisplacement inlay;
  std::span<const InlineTextDecoration> inlay_inline_texts;
  SDL_Color inlay_color{};  // fallback hint foreground when a hint carries none

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
