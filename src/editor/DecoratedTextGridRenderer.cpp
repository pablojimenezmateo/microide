#include "editor/DecoratedTextGridRenderer.h"
#include "render/SurfacePrimitives.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <optional>
#include <span>
#include <utility>

#include "util/StringUtil.h"

namespace microide::editor {

namespace {

bool SameColor(SDL_Color a, SDL_Color b) noexcept {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

std::optional<SDL_Color> OverrideForegroundAt(std::span<const TextStyleDecoration> overrides,
                                              std::size_t source_byte_column) {
  for (const TextStyleDecoration& ts : overrides) {
    if (ts.foreground.a == 0) {
      continue;
    }
    if ((ts.flags & kDecorationWholeLine) != 0) {
      return ts.foreground;
    }
    if (source_byte_column >= ts.start_column && source_byte_column < ts.end_column) {
      return ts.foreground;
    }
  }
  return std::nullopt;
}

}

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
                                 const std::vector<SyntaxTokenKind>& full_tokens,
                                 std::span<const TextStyleDecoration> foreground_overrides) {
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
  const auto effective_color_at = [&](std::size_t byte_offset) -> SDL_Color {
    if (!foreground_overrides.empty()) {
      if (const auto color = OverrideForegroundAt(foreground_overrides,
                                                  window.byte_offset + byte_offset)) {
        return *color;
      }
    }
    return SyntaxTokenColor(theme, token_kind_at(byte_offset), plain_color);
  };

  float segment_x = x;
  std::size_t segment_start = 0;
  SDL_Color color = effective_color_at(0);
  while (segment_start < window.text.size()) {
    std::size_t segment_end = segment_start;
    SDL_Color next_color = color;
    while (segment_end < window.text.size()) {
      const std::size_t next =
          segment_end + util::Utf8SequenceLength(window.text, segment_end);
      if (next >= window.text.size()) {
        segment_end = window.text.size();
        break;
      }
      const SDL_Color candidate = effective_color_at(next);
      if (!SameColor(candidate, color)) {
        segment_end = next;
        next_color = candidate;
        break;
      }
      segment_end = next;
    }

    const std::string_view segment_text(window.text.data() + segment_start,
                                        segment_end - segment_start);
    row.runs.push_back(DecoratedTextRun{
        .x = segment_x,
        .y = y,
        .color = color,
        .text = segment_text,
    });
    segment_x += text_renderer.MeasureWidth(segment_text);
    segment_start = segment_end;
    color = next_color;
  }
}

void AppendLayoutSyntaxTextRuns(DecoratedTextRow& row,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                float x,
                                float y,
                                const LayoutLine& layout,
                                SDL_Color plain_color,
                                const std::vector<SyntaxTokenKind>& full_tokens,
                                std::span<const TextStyleDecoration> foreground_overrides,
                                const InlayRowDisplacement& inlay) {
  const std::size_t visible_cells =
      std::min(layout.source_columns.size(), layout.text_offsets.size());
  if (layout.text.empty() || visible_cells == 0) {
    return;
  }

  const auto effective_color_at = [&](std::size_t cell) -> SDL_Color {
    const std::size_t source_column =
        cell < layout.source_columns.size() ? layout.source_columns[cell] : 0;
    if (!foreground_overrides.empty()) {
      if (const auto color = OverrideForegroundAt(foreground_overrides, source_column)) {
        return *color;
      }
    }
    const SyntaxTokenKind kind =
        source_column < full_tokens.size() ? full_tokens[source_column] : SyntaxTokenKind::Plain;
    return SyntaxTokenColor(theme, kind, plain_color);
  };

  const float char_width = text_renderer.CharWidth();
  const bool has_inlay = !inlay.empty();

  std::size_t segment_start = 0;
  SDL_Color color = effective_color_at(0);
  while (segment_start < visible_cells) {
    const std::size_t split_limit =
        has_inlay ? inlay.NextAnchorAtOrAfter(segment_start + 1)
                  : std::numeric_limits<std::size_t>::max();
    std::size_t segment_end = segment_start + 1;
    bool carried = false;
    SDL_Color next_color = color;
    while (segment_end < visible_cells && segment_end < split_limit) {
      const SDL_Color candidate = effective_color_at(segment_end);
      if (!SameColor(candidate, color)) {
        next_color = candidate;
        carried = true;
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
    const float inlay_shift =
        has_inlay ? static_cast<float>(inlay.CellsInsertedBefore(segment_start)) * char_width : 0.0f;
    row.runs.push_back(DecoratedTextRun{
        .x = x + static_cast<float>(segment_start) * char_width + inlay_shift,
        .y = y,
        .color = color,
        .text = segment_text,
    });
    segment_start = segment_end;
    if (segment_start < visible_cells) {
      color = carried ? next_color : effective_color_at(segment_start);
    }
  }
}

void AppendInlayHintRuns(DecoratedTextRow& row,
                         const render::TextRenderer& text_renderer,
                         float x,
                         float y,
                         float char_width,
                         float line_height,
                         std::span<const InlineTextDecoration> inline_texts,
                         std::span<const InlayCellSpan> spans,
                         SDL_Color fallback_color) {
  (void)text_renderer;
  std::size_t real_col = 0;
  std::size_t disp_col = 0;
  for (const InlayCellSpan& span : spans) {
    disp_col += span.anchor_visual_column - real_col;
    real_col = span.anchor_visual_column;
    if (span.source_index >= inline_texts.size()) {
      disp_col += span.cell_width;
      continue;
    }
    const InlineTextDecoration& inl = inline_texts[span.source_index];
    const float hint_left = x + static_cast<float>(disp_col) * char_width;
    if (inl.background.a != 0) {
      row.fills.push_back(DecoratedTextFill{
          .rect = SDL_FRect{hint_left, y - 1.0f, static_cast<float>(span.cell_width) * char_width,
                            line_height},
          .color = inl.background,
      });
    }
    const SDL_Color fg = inl.color.a != 0 ? inl.color : fallback_color;
    row.runs.push_back(DecoratedTextRun{.x = hint_left, .y = y, .color = fg, .text = inl.text});
    disp_col += span.cell_width;
  }
}

namespace {

void FlushFillRun(SDL_Renderer* renderer,
                  SDL_Color color,
                  std::vector<SDL_FRect>& batch) {
  if (batch.empty()) {
    return;
  }
  render::SetDrawColor(renderer, color);
  render::FillRectsInDrawColor(renderer, batch.data(), static_cast<int>(batch.size()));
  batch.clear();
}

template <class Item, class Project>
void RenderColorBatchedFills(SDL_Renderer* renderer,
                             std::vector<SDL_FRect>& batch,
                             std::span<const Item> items,
                             Project project) {
  batch.clear();
  SDL_Color active_color{};
  bool batch_open = false;
  for (const Item& item : items) {
    const auto [rect, color] = project(item);
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
      continue;
    }
    if (!batch_open || !SameColor(active_color, color)) {
      FlushFillRun(renderer, active_color, batch);
      active_color = color;
      batch_open = true;
    }
    batch.push_back(rect);
  }
  FlushFillRun(renderer, active_color, batch);
}

}

void DecoratedTextGridRenderer::RenderRow(SDL_Renderer* renderer,
                                          const render::TextRenderer& text_renderer,
                                          const DecoratedTextRow& row) const {
  if (renderer == nullptr) {
    return;
  }

  std::vector<SDL_FRect>& batch = fill_batch_scratch_;
  RenderColorBatchedFills<DecoratedTextFill>(
      renderer, batch, row.fills,
      [](const DecoratedTextFill& fill) {
        return std::pair<SDL_FRect, SDL_Color>{fill.rect, fill.color};
      });

  if (!row.runs.empty()) {
    text_renderer.DrawRuns(renderer, row.runs.data(), row.runs.size());
  }

  RenderColorBatchedFills<DecoratedUnderline>(
      renderer, batch, row.underlines,
      [](const DecoratedUnderline& underline) {
        const Uint8 dim_alpha = static_cast<Uint8>(
            std::clamp(std::lround(static_cast<double>(underline.color.a) * 0.55), 0l, 255l));
        return std::pair<SDL_FRect, SDL_Color>{
            underline.rect,
            SDL_Color{underline.color.r, underline.color.g, underline.color.b, dim_alpha}};
      });
}

}
