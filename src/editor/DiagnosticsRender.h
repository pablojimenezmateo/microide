#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsStore.h"
#include "render/Theme.h"

namespace microide::editor {

SDL_Color DiagnosticSeverityColor(const render::Theme& theme, DiagnosticSeverity severity);

std::optional<SDL_FRect> DiagnosticUnderlineRect(const render::TextRenderer& text_renderer,
                                                 float text_x,
                                                 float y,
                                                 float line_height,
                                                 const std::string& text,
                                                 std::size_t line_index,
                                                 std::size_t horizontal_scroll,
                                                 std::size_t visible_columns,
                                                 std::size_t tab_size,
                                                 const PublishedDiagnostic& diagnostic);

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
                                std::span<const PublishedDiagnostic> diagnostics);

}  // namespace microide::editor
