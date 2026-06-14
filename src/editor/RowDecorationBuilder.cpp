#include "editor/RowDecorationBuilder.h"

#include <algorithm>
#include <cmath>

#include "editor/DiagnosticsRender.h"

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
    row.underlines.push_back(DecoratedUnderline{
        .rect = SDL_FRect{start_x, y + line_height - 2.0f, span_width, 1.0f},
        .color = SDL_Color{underline_color.r, underline_color.g, underline_color.b,
                           static_cast<Uint8>(std::clamp(
                               std::lround(static_cast<double>(underline_color.a) * 0.55), 0l,
                               255l))},
    });
  }
}

namespace {

std::size_t ResolveVisualColumn(const RowDecorationInput& in, std::size_t column) {
  if (in.layout != nullptr) {
    return TextLayout::VisualColumnFromLayoutClipped(*in.layout, in.row_visual_start,
                                                     in.row_visual_end, column);
  }
  if (in.visual_map != nullptr) {
    return in.visual_map->VisualColumnFor(column);
  }
  return column;
}

void AppendColumnFill(DecoratedTextRow& row, const RowDecorationInput& in, const RowFillSpan& span) {
  if (span.geometry == RowFillSpan::Geometry::kSingleCell) {
    const std::size_t cell_visual = ResolveVisualColumn(in, span.start_column);
    if (cell_visual < in.row_visual_start || cell_visual >= in.row_visual_end) {
      return;
    }
    row.fills.push_back(DecoratedTextFill{
        .rect = SDL_FRect{
            in.text_x + static_cast<float>(cell_visual - in.row_visual_start) * in.char_width,
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
  row.fills.push_back(DecoratedTextFill{
      .rect = SDL_FRect{
          in.text_x + static_cast<float>(visible_start - in.row_visual_start) * in.char_width,
          in.y - 1.0f,
          static_cast<float>(visible_end - visible_start) * in.char_width,
          in.line_height,
      },
      .color = span.color,
  });
}

}  // namespace

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

  const std::string_view text_view = in.text != nullptr ? std::string_view(*in.text)
                                                        : std::string_view{};
  static const std::vector<SyntaxTokenKind> kNoTokens;
  const std::vector<SyntaxTokenKind>& tokens = in.tokens != nullptr ? *in.tokens : kNoTokens;
  if (in.text_renderer != nullptr && in.theme != nullptr) {
    if (in.layout != nullptr) {
      AppendLayoutSyntaxTextRuns(row, *in.text_renderer, *in.theme, in.text_x, in.y, *in.layout,
                                 in.plain_color, tokens);
    } else {
      const std::size_t visible_columns =
          in.row_visual_end > in.row_visual_start ? in.row_visual_end - in.row_visual_start : 0;
      AppendVisibleSyntaxTextRuns(row, *in.text_renderer, *in.theme, in.text_x, in.y, text_view,
                                  in.row_visual_start, visible_columns, in.plain_color, tokens);
    }

    if (!in.changed_spans.empty()) {
      const std::size_t visible_columns =
          in.row_visual_end > in.row_visual_start ? in.row_visual_end - in.row_visual_start : 0;
      AppendChangedSpanUnderlines(row, *in.text_renderer, in.text_x, in.y, in.line_height,
                                  text_view, in.row_visual_start, visible_columns, in.changed_spans,
                                  in.changed_span_color);
    }

    if (!in.diagnostics.empty() && in.text != nullptr) {
      AppendDiagnosticUnderlines(row, *in.text_renderer, *in.theme, in.text_x, in.y, in.line_height,
                                 *in.text, in.diagnostic_line_index, in.diagnostic_horizontal_scroll,
                                 in.diagnostic_visible_columns, in.tab_size, in.diagnostics);
    }
  }
}

}  // namespace microide::editor
