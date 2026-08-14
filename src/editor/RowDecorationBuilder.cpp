#include "editor/RowDecorationBuilder.h"

#include <algorithm>
#include <cmath>

#include "editor/DiagnosticsRender.h"
#include "util/StringUtil.h"

namespace microide::editor {

void AppendChangedSpanUnderlines(DecoratedTextRow& row,
                                 const render::TextRenderer& text_renderer,
                                 float text_x,
                                 float y,
                                 float line_height,
                                 std::string_view text,
                                 std::size_t horizontal_scroll,
                                 std::size_t visible_columns,
                                 std::span<const compare::CompareTextSpan> changed_spans,
                                 SDL_Color underline_color) {
  if (text.empty() || changed_spans.empty()) {
    return;
  }

  const VisibleTextWindow window = SliceVisibleColumns(text, horizontal_scroll, visible_columns);
  if (window.text.empty()) {
    return;
  }
  const std::size_t window_end = window.byte_offset + window.text.size();

  for (const auto& span : changed_spans) {
    if (span.end <= window.byte_offset) {
      continue;
    }
    if (span.start >= window_end) {
      break;
    }

    const std::size_t clipped_start = std::max(span.start, window.byte_offset);
    const std::size_t clipped_end = std::min(span.end, window_end);
    if (clipped_end <= clipped_start) {
      continue;
    }

    const std::size_t local_start = clipped_start - window.byte_offset;
    const std::size_t local_end = clipped_end - window.byte_offset;
    const std::string_view prefix_text(window.text.data(), local_start);
    const std::string_view changed_text(window.text.data() + local_start, local_end - local_start);
    const float start_x = text_x + text_renderer.MeasureWidth(prefix_text);
    const float span_width = text_renderer.MeasureWidth(changed_text);
    if (span_width <= 0.0f) {
      continue;
    }
    // Full-intensity source color: RenderRow applies the single 0.55 dim at draw time
    // (see AppendChangedSpanUnderlinesGrid) — pre-dimming here would fade it twice.
    row.underlines.push_back(DecoratedUnderline{
        .rect = SDL_FRect{start_x, y + line_height - 2.0f, span_width, 1.0f},
        .color = underline_color,
    });
  }
}

namespace {

std::size_t ResolveVisualColumn(const RowDecorationInput& in, std::size_t column) {
  return TextLayout::ResolveVisualColumn(in.layout, in.visual_map, in.row_visual_start,
                                         in.row_visual_end, column);
}

// Row-local display column of an absolute visual column, counting any phantom
// inlay-hint cells inserted before it. Identity when the row has no inlay hints.
std::size_t DisplayColumn(const RowDecorationInput& in, std::size_t absolute_visual) {
  const std::size_t local = absolute_visual - in.row_visual_start;
  return local + in.inlay.CellsInsertedBefore(local);
}

void AppendColumnFill(DecoratedTextRow& row, const RowDecorationInput& in, const RowFillSpan& span) {
  if (span.geometry == RowFillSpan::Geometry::kSingleCell) {
    const std::size_t cell_visual = ResolveVisualColumn(in, span.start_column);
    if (cell_visual < in.row_visual_start || cell_visual >= in.row_visual_end) {
      return;
    }
    row.fills.push_back(DecoratedTextFill{
        .rect = SDL_FRect{
            in.text_x + static_cast<float>(DisplayColumn(in, cell_visual)) * in.char_width,
            in.y - 1.0f,
            in.char_width,
            in.line_height,
        },
        .color = span.color,
    });
    return;
  }

  const std::size_t start_visual = ResolveVisualColumn(in, span.start_column);
  const std::size_t end_visual = ResolveVisualColumn(in, span.end_column);
  const std::size_t visible_start = std::max(start_visual, in.row_visual_start);
  const std::size_t visible_end = std::min(end_visual, in.row_visual_end);
  if (visible_end <= visible_start) {
    return;
  }
  const std::size_t display_start = DisplayColumn(in, visible_start);
  const std::size_t display_end = DisplayColumn(in, visible_end);
  row.fills.push_back(DecoratedTextFill{
      .rect = SDL_FRect{
          in.text_x + static_cast<float>(display_start) * in.char_width,
          in.y - 1.0f,
          static_cast<float>(display_end - display_start) * in.char_width,
          in.line_height,
      },
      .color = span.color,
  });
}

// Changed-span underlines positioned on the fixed cell grid (source->visual via
// ResolveVisualColumn, then visual * char_width), matching the grid text runs,
// caret, selection and diagnostics. Used when the row renders on the grid
// (`layout` set); the sibling AppendChangedSpanUnderlines keeps the MeasureWidth
// geometry for the proportional (layout == null) path so each row stays internally
// consistent with however its text was drawn.
void AppendChangedSpanUnderlinesGrid(DecoratedTextRow& row, const RowDecorationInput& in) {
  // Push the source color at full intensity: DecoratedTextGridRenderer::RenderRow dims
  // EVERY underline's alpha by 0.55 at draw time, so pre-dimming here would apply the
  // fade twice (~0.30 alpha) and make diff underlines fainter than diagnostic squiggles
  // and plugin underlines on the same row (both of which push full alpha).
  const SDL_Color color = in.changed_span_color;
  for (const compare::CompareTextSpan& span : in.changed_spans) {
    const std::size_t start_visual = ResolveVisualColumn(in, span.start);
    const std::size_t end_visual = ResolveVisualColumn(in, span.end);
    const std::size_t visible_start = std::max(start_visual, in.row_visual_start);
    const std::size_t visible_end = std::min(end_visual, in.row_visual_end);
    if (visible_end <= visible_start) {
      continue;
    }
    // Inlay displacement is applied uniformly to every underline in a single
    // post-pass in BuildDecoratedRow, so this stays grid-plain.
    row.underlines.push_back(DecoratedUnderline{
        .rect = SDL_FRect{
            in.text_x + static_cast<float>(visible_start - in.row_visual_start) * in.char_width,
            in.y + in.line_height - 2.0f,
            static_cast<float>(visible_end - visible_start) * in.char_width,
            1.0f},
        .color = color,
    });
  }
}

// Append underline / strikethrough lines for plugin text-style decorations,
// clipped to the visible byte window (same geometry as diagnostic underlines).
void AppendTextStyleUnderlines(DecoratedTextRow& row, const RowDecorationInput& in) {
  if (in.text_styles.empty() || in.text.empty() || in.text_renderer == nullptr) {
    return;
  }
  const std::size_t visible_columns =
      in.row_visual_end > in.row_visual_start ? in.row_visual_end - in.row_visual_start : 0;
  const VisibleTextWindow window =
      SliceVisibleColumns(in.text, in.row_visual_start, visible_columns);
  if (window.text.empty()) {
    return;
  }
  const std::size_t window_end = window.byte_offset + window.text.size();
  for (const TextStyleDecoration& ts : in.text_styles) {
    const bool underline = (ts.flags & kDecorationUnderline) != 0;
    const bool strike = (ts.flags & kDecorationStrikethrough) != 0;
    if (!underline && !strike) {
      continue;
    }
    const bool whole = (ts.flags & kDecorationWholeLine) != 0;
    const std::size_t start = whole ? 0 : ts.start_column;
    const std::size_t end = whole ? in.line_length : ts.end_column;
    if (end <= start || end <= window.byte_offset || start >= window_end) {
      continue;
    }
    const std::size_t clipped_start = std::max(start, window.byte_offset);
    const std::size_t clipped_end = std::min(end, window_end);
    if (clipped_end <= clipped_start) {
      continue;
    }
    const std::size_t local_start = clipped_start - window.byte_offset;
    const std::size_t local_end = clipped_end - window.byte_offset;
    const std::string_view prefix_text(window.text.data(), local_start);
    const std::string_view span_text(window.text.data() + local_start, local_end - local_start);
    const float start_x = in.text_x + in.text_renderer->MeasureWidth(prefix_text);
    const float span_width = in.text_renderer->MeasureWidth(span_text);
    if (span_width <= 0.0f) {
      continue;
    }
    SDL_Color color = ts.line_color.a != 0 ? ts.line_color
                      : ts.foreground.a != 0 ? ts.foreground
                                             : in.plain_color;
    // Underlines are dimmed 0.55x at draw time; lift the source alpha so plugin
    // lines render at the author's intended intensity.
    color.a = 255;
    if (underline) {
      row.underlines.push_back(DecoratedUnderline{
          .rect = SDL_FRect{start_x, in.y + in.line_height - 2.0f, span_width, 1.0f},
          .color = color,
      });
    }
    if (strike) {
      row.underlines.push_back(DecoratedUnderline{
          .rect = SDL_FRect{start_x, in.y + in.line_height * 0.5f, span_width, 1.0f},
          .color = color,
      });
    }
  }
}

// Grid-aligned plugin text-style under/strike lines (source->visual via
// ResolveVisualColumn, then visual * char_width), matching the grid text runs,
// caret, selection, diagnostics and the sibling AppendChangedSpanUnderlinesGrid.
// Used when the row renders on the fixed cell grid (`layout` set): the proportional
// MeasureWidth geometry of AppendTextStyleUnderlines mispositions the line whenever
// the row has a tab, a wide glyph, or is horizontally scrolled (row_visual_start is
// a visual column, not a codepoint count). Inlay displacement is applied uniformly
// by the post-pass in BuildDecoratedRow, so this stays grid-plain.
void AppendTextStyleUnderlinesGrid(DecoratedTextRow& row, const RowDecorationInput& in) {
  if (in.text_styles.empty()) {
    return;
  }
  for (const TextStyleDecoration& ts : in.text_styles) {
    const bool underline = (ts.flags & kDecorationUnderline) != 0;
    const bool strike = (ts.flags & kDecorationStrikethrough) != 0;
    if (!underline && !strike) {
      continue;
    }
    const bool whole = (ts.flags & kDecorationWholeLine) != 0;
    const std::size_t start_col = whole ? 0 : ts.start_column;
    const std::size_t end_col = whole ? in.line_length : ts.end_column;
    if (end_col <= start_col) {
      continue;
    }
    const std::size_t start_visual = ResolveVisualColumn(in, start_col);
    const std::size_t end_visual = ResolveVisualColumn(in, end_col);
    const std::size_t visible_start = std::max(start_visual, in.row_visual_start);
    const std::size_t visible_end = std::min(end_visual, in.row_visual_end);
    if (visible_end <= visible_start) {
      continue;
    }
    SDL_Color color = ts.line_color.a != 0 ? ts.line_color
                      : ts.foreground.a != 0 ? ts.foreground
                                             : in.plain_color;
    color.a = 255;
    const float x =
        in.text_x + static_cast<float>(visible_start - in.row_visual_start) * in.char_width;
    const float width = static_cast<float>(visible_end - visible_start) * in.char_width;
    if (underline) {
      row.underlines.push_back(DecoratedUnderline{
          .rect = SDL_FRect{x, in.y + in.line_height - 2.0f, width, 1.0f},
          .color = color,
      });
    }
    if (strike) {
      row.underlines.push_back(DecoratedUnderline{
          .rect = SDL_FRect{x, in.y + in.line_height * 0.5f, width, 1.0f},
          .color = color,
      });
    }
  }
}

}  // namespace

void PushWhitespaceMarker(std::vector<DecoratedTextFill>& fills,
                          bool is_tab,
                          float cell_x,
                          float char_width,
                          float cell_span_width,
                          float y,
                          float line_height,
                          SDL_Color color) {
  if (is_tab) {
    fills.push_back(DecoratedTextFill{
        .rect = SDL_FRect{cell_x + 2.0f, y + line_height * 0.5f, cell_span_width - 4.0f, 1.0f},
        .color = color,
    });
  } else {
    fills.push_back(DecoratedTextFill{
        .rect = SDL_FRect{cell_x + char_width * 0.5f - 1.0f, y + line_height * 0.5f - 1.0f, 2.0f,
                          2.0f},
        .color = color,
    });
  }
}

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
                                    CellShiftRef shift_px) {
  std::size_t visual_col = 0;
  std::size_t i = 0;
  // Resume at the row rather than at byte 0 when everything before it is plain
  // single-cell ASCII: there byte offset IS visual column (TD-2026-08-12-187).
  const std::size_t prefix_probe = std::min(row_visual_start, text.size());
  if (util::FirstNonAsciiOrByte(text.substr(0, prefix_probe), '\t') >= prefix_probe) {
    i = prefix_probe;
    visual_col = prefix_probe;
  }
  const std::size_t start_byte = i;
  for (; i < text.size();) {
    const char c = text[i];
    i += util::Utf8SequenceLength(text, i);
    const std::size_t cell_start = visual_col;
    visual_col = TextLayout::AdvanceVisualColumn(cell_start, c, tab_size);
    const std::size_t cell_width = visual_col - cell_start;
    if (cell_start >= row_visual_end) {
      break;
    }
    if (cell_start < row_visual_start || visual_col > row_visual_end) {
      continue;
    }
    if (c != ' ' && c != '\t') {
      continue;
    }
    PushWhitespaceMarker(fills, c == '\t',
                         text_x + static_cast<float>(cell_start - row_visual_start) * char_width +
                             shift_px(cell_start),
                         char_width, static_cast<float>(cell_width) * char_width, y, line_height,
                         color);
  }
  return i - start_byte;
}

std::size_t AppendWhitespaceMarkers(std::vector<DecoratedTextFill>& fills,
                                    std::string_view text,
                                    std::size_t tab_size,
                                    std::size_t row_visual_start,
                                    std::size_t row_visual_end,
                                    float text_x,
                                    float char_width,
                                    float y,
                                    float line_height,
                                    SDL_Color color) {
  const auto no_shift = [](std::size_t) { return 0.0f; };
  return AppendWhitespaceMarkers(fills, text, tab_size, row_visual_start, row_visual_end, text_x,
                                 char_width, y, line_height, color, no_shift);
}

void BuildDecoratedRow(DecoratedTextRow& row, const RowDecorationInput& in) {
  row.fills.clear();
  row.runs.clear();
  row.underlines.clear();

  if (in.has_background_fill) {
    row.fills.push_back(in.background_fill);
  }
  if (in.has_edge_stripe) {
    row.fills.push_back(in.edge_stripe_fill);
  }
  for (const RowFillSpan& span : in.column_fills) {
    AppendColumnFill(row, in, span);
  }
  for (const DecoratedTextFill& fill : in.prepositioned_fills) {
    row.fills.push_back(fill);
  }
  // Plugin text-style background fills ride the column-fill geometry path,
  // layered above selection/search so a translucent author color blends over
  // them and below the syntax runs.
  for (const TextStyleDecoration& ts : in.text_styles) {
    if (ts.background.a == 0) {
      continue;
    }
    const bool whole = (ts.flags & kDecorationWholeLine) != 0;
    RowFillSpan span{
        .start_column = whole ? 0 : ts.start_column,
        .end_column = whole ? in.line_length : ts.end_column,
        .color = ts.background,
        .geometry = RowFillSpan::Geometry::kRange,
    };
    if (span.end_column > span.start_column) {
      AppendColumnFill(row, in, span);
    }
  }

  const std::string_view text_view = in.text;
  static const std::vector<SyntaxTokenKind> kNoTokens;
  const std::vector<SyntaxTokenKind>& tokens = in.tokens != nullptr ? *in.tokens : kNoTokens;
  if (in.text_renderer != nullptr && in.theme != nullptr) {
    if (in.layout != nullptr) {
      AppendLayoutSyntaxTextRuns(row, *in.text_renderer, *in.theme, in.text_x, in.y, *in.layout,
                                 in.plain_color, tokens, in.text_styles, in.inlay);
      // Draw the hint glyphs in the phantom cells the displacement reserved.
      if (!in.inlay.empty()) {
        AppendInlayHintRuns(row, *in.text_renderer, in.text_x, in.y, in.char_width, in.line_height,
                            in.inlay_inline_texts, in.inlay.spans(), in.inlay_color);
      }
    } else {
      const std::size_t visible_columns =
          in.row_visual_end > in.row_visual_start ? in.row_visual_end - in.row_visual_start : 0;
      AppendVisibleSyntaxTextRuns(row, *in.text_renderer, *in.theme, in.text_x, in.y, text_view,
                                  in.row_visual_start, visible_columns, in.plain_color, tokens,
                                  in.text_styles);
    }

    if (!in.changed_spans.empty()) {
      if (in.layout != nullptr) {
        // Grid text -> grid-aligned underlines (see AppendChangedSpanUnderlinesGrid).
        AppendChangedSpanUnderlinesGrid(row, in);
      } else {
        const std::size_t visible_columns =
            in.row_visual_end > in.row_visual_start ? in.row_visual_end - in.row_visual_start : 0;
        AppendChangedSpanUnderlines(row, *in.text_renderer, in.text_x, in.y, in.line_height,
                                    text_view, in.row_visual_start, visible_columns, in.changed_spans,
                                    in.changed_span_color);
      }
    }

    if (!in.diagnostics.empty() && !in.text.empty()) {  // `text` is set only when needed
      AppendDiagnosticUnderlines(row, *in.text_renderer, *in.theme, in.text_x, in.y, in.line_height,
                                 in.text, in.diagnostic_line_index, in.diagnostic_horizontal_scroll,
                                 in.diagnostic_visible_columns, in.tab_size, in.diagnostics);
    }

    if (in.layout != nullptr) {
      // Grid text -> grid-aligned under/strike lines (see AppendTextStyleUnderlinesGrid).
      AppendTextStyleUnderlinesGrid(row, in);
    } else {
      AppendTextStyleUnderlines(row, in);
    }
  }

  // Mid-line inlay hints shift the real glyphs right; every underline (diagnostic
  // squiggle, semantic/plugin under/strike) must follow the token it sits under.
  // Fills and runs are already displaced at construction; underlines are the one
  // family built with proportional geometry across several appenders, so shift them
  // here in one uniform pass keyed on each underline's start column. Identity (and
  // skipped) when the row has no hints.
  if (!in.inlay.empty() && in.char_width > 0.0f) {
    for (DecoratedUnderline& underline : row.underlines) {
      const float local_cells = (underline.rect.x - in.text_x) / in.char_width;
      if (local_cells < 0.0f) {
        continue;
      }
      const std::size_t local_visual = static_cast<std::size_t>(local_cells + 0.5f);
      underline.rect.x +=
          static_cast<float>(in.inlay.CellsInsertedBefore(local_visual)) * in.char_width;
    }
  }
}

}  // namespace microide::editor
