#include "editor/DecoratedTextGridRenderer.h"

#include <algorithm>
#include <cmath>

#include "util/StringUtil.h"

namespace microide::editor {

VisibleTextWindow SliceVisibleColumns(std::string_view text,
                                      std::size_t start_column,
                                      std::size_t visible_columns) {
  const std::size_t byte_offset = util::Utf8ByteOffsetForCodepointCount(text, start_column);
  const std::size_t byte_length =
      util::Utf8ByteOffsetForCodepointCount(text.substr(byte_offset), visible_columns);
  return VisibleTextWindow{
      .text = text.substr(byte_offset, byte_length),
      .byte_offset = byte_offset,
  };
}

SDL_Color SyntaxTokenColor(const render::Theme& theme,
                           SyntaxTokenKind kind,
                           SDL_Color fallback) {
  switch (kind) {
    case SyntaxTokenKind::Keyword:
      return theme.syntax_keyword;
    case SyntaxTokenKind::Type:
      return theme.syntax_type;
    case SyntaxTokenKind::String:
      return theme.syntax_string;
    case SyntaxTokenKind::Comment:
      return theme.syntax_comment;
    case SyntaxTokenKind::Number:
      return theme.syntax_number;
    case SyntaxTokenKind::Constant:
      return theme.syntax_constant;
    case SyntaxTokenKind::Preprocessor:
      return theme.syntax_preprocessor;
    case SyntaxTokenKind::Operator:
      return theme.syntax_operator;
    case SyntaxTokenKind::Plain:
    default:
      return fallback;
  }
}

void AppendVisibleSyntaxTextRuns(DecoratedTextRow& row,
                                 const render::TextRenderer& text_renderer,
                                 const render::Theme& theme,
                                 float x,
                                 float y,
                                 std::string_view text,
                                 std::size_t horizontal_scroll,
                                 std::size_t visible_columns,
                                 SDL_Color plain_color,
                                 const std::vector<SyntaxTokenKind>& full_tokens) {
  if (text.empty()) {
    return;
  }

  const VisibleTextWindow window =
      SliceVisibleColumns(text, horizontal_scroll, visible_columns);
  if (window.text.empty()) {
    return;
  }

  const auto token_kind_at = [&](std::size_t byte_offset) {
    const std::size_t absolute_offset = window.byte_offset + byte_offset;
    if (absolute_offset < full_tokens.size()) {
      return full_tokens[absolute_offset];
    }
    return SyntaxTokenKind::Plain;
  };

  float segment_x = x;
  for (std::size_t segment_start = 0; segment_start < window.text.size();) {
    const SyntaxTokenKind kind = token_kind_at(segment_start);
    std::size_t segment_end = segment_start;
    while (segment_end < window.text.size()) {
      const std::size_t next =
          segment_end + util::Utf8SequenceLength(window.text, segment_end);
      if (next >= window.text.size()) {
        segment_end = window.text.size();
        break;
      }
      if (token_kind_at(next) != kind) {
        segment_end = next;
        break;
      }
      segment_end = next;
    }

    const std::string_view segment_text(window.text.data() + segment_start,
                                        segment_end - segment_start);
    row.runs.push_back(DecoratedTextRun{
        .x = segment_x,
        .y = y,
        .color = SyntaxTokenColor(theme, kind, plain_color),
        .text = segment_text,
    });
    segment_x += text_renderer.MeasureWidth(segment_text);
    segment_start = segment_end;
  }
}

void AppendLayoutSyntaxTextRuns(DecoratedTextRow& row,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                float x,
                                float y,
                                const LayoutLine& layout,
                                SDL_Color plain_color,
                                const std::vector<SyntaxTokenKind>& full_tokens) {
  const std::size_t visible_cells =
      std::min(layout.source_columns.size(), layout.text_offsets.size());
  if (layout.text.empty() || visible_cells == 0) {
    return;
  }

  for (std::size_t segment_start = 0; segment_start < visible_cells;) {
    const std::size_t source_column =
        segment_start < layout.source_columns.size() ? layout.source_columns[segment_start] : 0;
    const SyntaxTokenKind kind =
        source_column < full_tokens.size() ? full_tokens[source_column] : SyntaxTokenKind::Plain;

    std::size_t segment_end = segment_start + 1;
    while (segment_end < visible_cells) {
      const std::size_t next_source_column =
          segment_end < layout.source_columns.size() ? layout.source_columns[segment_end] : 0;
      const SyntaxTokenKind next_kind =
          next_source_column < full_tokens.size() ? full_tokens[next_source_column]
                                                  : SyntaxTokenKind::Plain;
      if (next_kind != kind) {
        break;
      }
      ++segment_end;
    }

    const std::size_t segment_text_start = layout.text_offsets[segment_start];
    const std::size_t segment_text_end =
        segment_end < layout.text_offsets.size() ? layout.text_offsets[segment_end]
                                                 : layout.text.size();
    const std::string_view segment_text(layout.text.data() + segment_text_start,
                                        segment_text_end - segment_text_start);
    row.runs.push_back(DecoratedTextRun{
        .x = x + static_cast<float>(segment_start) * text_renderer.CharWidth(),
        .y = y,
        .color = SyntaxTokenColor(theme, kind, plain_color),
        .text = segment_text,
    });
    segment_start = segment_end;
  }
}

void DecoratedTextGridRenderer::RenderRow(SDL_Renderer* renderer,
                                          const render::TextRenderer& text_renderer,
                                          const DecoratedTextRow& row) const {
  if (renderer == nullptr) {
    return;
  }

  for (const DecoratedTextFill& fill : row.fills) {
    if (fill.rect.w <= 0.0f || fill.rect.h <= 0.0f) {
      continue;
    }
    SDL_SetRenderDrawColor(renderer, fill.color.r, fill.color.g, fill.color.b, fill.color.a);
    SDL_RenderFillRect(renderer, &fill.rect);
  }

  for (const DecoratedTextRun& run : row.runs) {
    if (run.text.empty()) {
      continue;
    }
    text_renderer.DrawString(renderer, run.x, run.y, run.color, run.text);
  }

  for (const DecoratedUnderline& underline : row.underlines) {
    if (underline.rect.w <= 0.0f || underline.rect.h <= 0.0f) {
      continue;
    }
    const Uint8 dim_alpha =
        static_cast<Uint8>(std::clamp(std::lround(static_cast<double>(underline.color.a) * 0.55),
                                      0l, 255l));
    SDL_SetRenderDrawColor(renderer, underline.color.r, underline.color.g, underline.color.b,
                           dim_alpha);
    SDL_RenderFillRect(renderer, &underline.rect);
  }
}

}  // namespace microide::editor
