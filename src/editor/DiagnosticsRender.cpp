#include "editor/DiagnosticsRender.h"

#include <algorithm>

namespace microide::editor {

SDL_Color DiagnosticSeverityColor(const render::Theme& theme, DiagnosticSeverity severity) {
  switch (severity) {
    case DiagnosticSeverity::Error:
      return theme.diagnostic_error;
    case DiagnosticSeverity::Warning:
      return theme.diagnostic_warning;
    case DiagnosticSeverity::Info:
      return theme.diagnostic_info;
    case DiagnosticSeverity::Hint:
      return theme.diagnostic_hint;
  }
  return theme.diagnostic_error;
}

std::optional<SDL_FRect> DiagnosticUnderlineRect(const render::TextRenderer& text_renderer,
                                                 float text_x,
                                                 float y,
                                                 float line_height,
                                                 const std::string& text,
                                                 std::size_t line_index,
                                                 std::size_t horizontal_scroll,
                                                 std::size_t visible_columns,
                                                 std::size_t tab_size,
                                                 const PublishedDiagnostic& diagnostic) {
  if (line_height <= 0.0f || visible_columns == 0 ||
      (line_index < diagnostic.range.start.line || line_index > diagnostic.range.end.line)) {
    return std::nullopt;
  }

  const std::size_t raw_start_column =
      line_index == diagnostic.range.start.line ? diagnostic.range.start.column : 0;
  const std::size_t raw_end_column =
      line_index == diagnostic.range.end.line ? diagnostic.range.end.column : text.size();
  const std::size_t clamped_start = std::min(raw_start_column, text.size());
  const std::size_t clamped_end = std::min(raw_end_column, text.size());

  std::size_t start_visual =
      TextLayout::VisualColumnForTextColumn(text, clamped_start, tab_size);
  std::size_t end_visual =
      TextLayout::VisualColumnForTextColumn(text, clamped_end, tab_size);
  if (end_visual <= start_visual) {
    end_visual = start_visual + 1;
  }

  const std::size_t visible_end_column = horizontal_scroll + visible_columns;
  if (end_visual <= horizontal_scroll || start_visual >= visible_end_column) {
    return std::nullopt;
  }

  const std::size_t visible_start = std::max(start_visual, horizontal_scroll);
  const std::size_t visible_end = std::min(end_visual, visible_end_column);
  if (visible_end <= visible_start) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  return SDL_FRect{
      text_x + static_cast<float>(visible_start - horizontal_scroll) * char_width,
      y + line_height - 2.0f,
      std::max(1.0f, static_cast<float>(visible_end - visible_start) * char_width),
      1.0f,
  };
}

void AppendDiagnosticUnderlines(DecoratedTextRow& row,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                float text_x,
                                float y,
                                float line_height,
                                const std::string& text,
                                std::size_t line_index,
                                std::size_t horizontal_scroll,
                                std::size_t visible_columns,
                                std::size_t tab_size,
                                std::span<const PublishedDiagnostic> diagnostics) {
  if (diagnostics.empty() || line_height <= 0.0f || visible_columns == 0) {
    return;
  }

  for (const PublishedDiagnostic& diagnostic : diagnostics) {
    const auto rect = DiagnosticUnderlineRect(text_renderer, text_x, y, line_height, text,
                                              line_index, horizontal_scroll, visible_columns,
                                              tab_size, diagnostic);
    if (!rect.has_value()) {
      continue;
    }

    row.underlines.push_back(DecoratedUnderline{
        .rect = *rect,
        .color = DiagnosticSeverityColor(theme, diagnostic.severity),
    });
  }
}

}  // namespace microide::editor
