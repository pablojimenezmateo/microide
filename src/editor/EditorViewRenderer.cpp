#include "editor/EditorViewRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "editor/DecoratedTextGridRenderer.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {

namespace {

const DecoratedTextGridRenderer kDecoratedRowRenderer;

float ComputeGutterWidth(const render::TextRenderer& text_renderer, std::size_t line_count) {
  const std::string last_line_label = std::to_string(std::max<std::size_t>(1, line_count));
  return std::max(48.0f, text_renderer.MeasureWidth(last_line_label) + 18.0f);
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

void DrawPlaceholderView(SDL_Renderer* renderer,
                         const render::TextRenderer& text_renderer,
                         const render::Theme& theme,
                         const SDL_FRect& rect) {
  struct CheatRow {
    std::string_view key;
    std::string_view label;
  };

  const auto draw_panel = [&](const SDL_FRect& panel,
                              std::string_view title,
                              auto&& rows,
                              float key_width) {
    SDL_SetRenderDrawColor(renderer, theme.surface_background.r, theme.surface_background.g,
                           theme.surface_background.b, theme.surface_background.a);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    SDL_RenderRect(renderer, &panel);

    const float inset_x = panel.x + 12.0f;
    const float title_y = panel.y + 10.0f;
    const float row_y_start = title_y + text_renderer.LineHeight() + 8.0f;
    const float row_step = text_renderer.LineHeight() + 1.0f;

    text_renderer.DrawStringOn(renderer, inset_x, title_y, theme.surface_text,
                               theme.surface_background, title);

    float row_y = row_y_start;
    for (const auto& row : rows) {
      if (row_y + text_renderer.LineHeight() > panel.y + panel.h - 10.0f) {
        break;
      }

      if (!row.label.empty() && key_width > 0.0f) {
        text_renderer.DrawStringOn(
            renderer, inset_x, row_y, theme.surface_text, theme.surface_background,
            text_renderer.TruncateToWidth(row.key, key_width - 8.0f));
        text_renderer.DrawStringOn(
            renderer, inset_x + key_width, row_y, theme.text_secondary,
            theme.surface_background,
            text_renderer.TruncateToWidth(row.label, panel.w - key_width - 24.0f));
      } else {
        text_renderer.DrawStringOn(
            renderer, inset_x, row_y, theme.text_secondary, theme.surface_background,
            text_renderer.TruncateToWidth(row.key, panel.w - 24.0f));
      }
      row_y += row_step;
    }
  };

  static constexpr std::array<CheatRow, 19> kCoreShortcuts = {{
      {"Ctrl+E", "command palette"},
      {"F6", "file finder overlay"},
      {"F8", "toggle sidebar"},
      {"Ctrl+0 / - / =", "reset / shrink / grow UI"},
      {"Ctrl+Tab", "switch editor/sidebar/panel focus"},
      {"Ctrl+S", "save active file"},
      {"Ctrl+W", "close active tab"},
      {"Ctrl+Shift+F", "project search sidebar"},
      {"Ctrl+F", "search in buffer"},
      {"Ctrl+H", "replace in buffer"},
      {"Ctrl+A", "select all"},
      {"Ctrl+C / X / V", "copy / cut / paste"},
      {"Ctrl+Z", "undo"},
      {"Ctrl+Shift+Z / Ctrl+Y", "redo"},
      {"Arrows + Shift", "move / extend selection"},
      {"Home / End", "line start / line end"},
      {"Ctrl+Home / End", "file start / file end"},
      {"PgUp / PgDn / Esc", "page move / close overlay"},
  }};

  static constexpr std::array<CheatRow, 40> kToolShortcuts = {{
      {"Tree Up / Down", "move tree selection"},
      {"Tree Left / Right", "collapse / expand"},
      {"Tree Enter", "open file or toggle dir"},
      {"Tree R", "refresh tree"},
      {"Tree D", "compare selected file"},
      {"Finder type", "filter project files"},
      {"Finder Backspace", "delete query char"},
      {"Finder Up / Down / Pg", "move result selection"},
      {"Finder Enter", "open selected file"},
      {"Search Up / Down / Home / End", "move result selection"},
      {"Search PgUp / PgDn", "page through results"},
      {"Search Enter / Right", "open selected result"},
      {"Search /", "edit query"},
      {"Search =", "edit replace text"},
      {"Search buttons", "toggle Lit/Rx, case mode, and hidden files"},
      {"Search r", "rerun current query"},
      {"Search R", "replace all literal matches"},
      {"Search Esc", "close temporary search"},
      {"Buffer search type", "filter buffer matches"},
      {"Buffer search Backspace", "delete query char"},
      {"Buffer search Up / Down / Pg", "move match selection"},
      {"Buffer search Enter", "jump to match and close"},
      {"Buffer replace Tab", "switch query / replace field"},
      {"Buffer replace Enter", "replace current match"},
      {"Buffer replace Ctrl+Enter", "replace all matches"},
      {"Compare j / k or arrows", "move compare rows"},
      {"Compare [ / ]", "jump previous / next hunk"},
      {"Compare Enter / o", "open working file"},
      {"Compare Esc", "close compare tab"},
      {"Merge Alt+[ / Alt+]", "move previous / next conflict"},
      {"Merge Alt+I / B / C / M", "take incoming / base / current / both"},
      {"Merge type / edit", "edit the result pane directly"},
      {"Merge click / drag", "select text or resize merge panes"},
      {"Merge Alt+O / Esc", "open result / close merge tab"},
      {"Split click", "focus the hovered editor split"},
      {"Split divider drag", "resize editor split"},
      {"Terminal click", "focus the terminal panel"},
      {"Terminal type / Enter", "send text / run command"},
      {"Terminal arrows / Pg / Home", "send terminal navigation keys"},
      {"Dirty prompt Tab / Enter / Esc", "pick / confirm / cancel"},
  }};

  static constexpr std::array<CheatRow, 38> kCommands = {{
      {"colorscheme [name]", ""},
      {"ui-scale [n|up|down|reset]", ""},
      {"help", ""},
      {"open <path>", ""},
      {"tab [path]", ""},
      {"tabswitch <tab>", ""},
      {"tabmove <n>", ""},
      {"compare [path] [commit]", ""},
      {"merge <base> <incoming> <current> [output]", ""},
      {"tab-size [n]", ""},
      {"indent-width [n]", ""},
      {"soft-tabs [on|off]", ""},
      {"save", ""},
      {"quit", ""},
      {"jump <line[:col]>", ""},
      {"vsplit [path]", ""},
      {"unsplit", ""},
      {"split-next", ""},
      {"split-prev", ""},
      {"split-first", ""},
      {"split-last", ""},
      {"term [command]", ""},
      {"find <query>", ""},
      {"files [root]", ""},
      {"tree [root]", ""},
      {"search <query>", ""},
      {"project-search [query]", ""},
      {"goto <line[:col]>", ""},
      {"tree-refresh", ""},
      {"sidebar-toggle [tool]", ""},
      {"sidebar-show [tool]", ""},
      {"sidebar-hide", ""},
      {"sidebar-close", ""},
      {"sidebar-width <n>", ""},
      {"focus <editor|sidebar|panel>", ""},
  }};

  const float card_width = std::min(rect.w - 48.0f, 1180.0f);
  const float card_height = std::min(rect.h - 48.0f, 620.0f);
  const float card_x = rect.x + std::max(24.0f, (rect.w - card_width) * 0.5f);
  const float card_y = rect.y + std::max(24.0f, rect.h * 0.05f);
  const SDL_FRect card = SDL_FRect{card_x, card_y, std::max(320.0f, card_width), card_height};
  const SDL_FRect accent = SDL_FRect{card.x, card.y, card.w, 3.0f};

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &card);
  SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
  SDL_RenderRect(renderer, &card);
  SDL_SetRenderDrawColor(renderer, theme.accent.r, theme.accent.g, theme.accent.b, theme.accent.a);
  SDL_RenderFillRect(renderer, &accent);

  const float inset_x = card.x + 18.0f;
  text_renderer.DrawStringOn(renderer, inset_x, card.y + 18.0f, theme.text_primary,
                             theme.surface_raised, "workspace ready");
  text_renderer.DrawStringOn(renderer, inset_x, card.y + 40.0f, theme.text_secondary,
                             theme.surface_raised,
                             "Open a file from the tree or use this cheat sheet while the editor is empty.");
  text_renderer.DrawStringOn(renderer, inset_x, card.y + 58.0f, theme.text_muted,
                             theme.surface_raised,
                             "Everything implemented today is listed below: commands on the right, shortcuts on the left.");

  const float panels_y = card.y + 92.0f;
  const float panels_h = card.h - 126.0f;
  const float panel_gap = 12.0f;
  const float panel_w = (card.w - 36.0f - panel_gap * 2.0f) / 3.0f;
  const SDL_FRect core_panel = SDL_FRect{card.x + 12.0f, panels_y, panel_w, panels_h};
  const SDL_FRect tool_panel =
      SDL_FRect{core_panel.x + core_panel.w + panel_gap, panels_y, panel_w, panels_h};
  const SDL_FRect command_panel =
      SDL_FRect{tool_panel.x + tool_panel.w + panel_gap, panels_y, panel_w, panels_h};

  draw_panel(core_panel, "Core Shortcuts", kCoreShortcuts, 128.0f);
  draw_panel(tool_panel, "Tool Shortcuts", kToolShortcuts, 148.0f);
  draw_panel(command_panel, "Command Palette", kCommands, 0.0f);

  const std::string backend_label = "text renderer: " + std::string(text_renderer.BackendName());
  const float backend_width = text_renderer.MeasureWidth(backend_label);
  text_renderer.DrawStringOn(renderer, card.x + card.w - backend_width - 18.0f,
                             card.y + card.h - 24.0f, theme.text_disabled,
                             theme.surface_raised, backend_label);
}

}  // namespace

EditorViewMetrics EditorViewRenderer::ComputeMetrics(const render::TextRenderer& text_renderer,
                                                     const TextViewport& viewport,
                                                     const SDL_FRect& rect) {
  EditorViewMetrics metrics;
  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  metrics.gutter_width = ComputeGutterWidth(text_renderer, viewport.line_count());
  metrics.text_x = rect.x + metrics.gutter_width + 12.0f;
  metrics.first_line_y = rect.y + 8.0f;
  metrics.line_height = text_renderer.LineHeight();
  metrics.visible_rows = static_cast<std::size_t>(
      std::max(1, static_cast<int>((rect.h - 12.0f) / metrics.line_height)));
  metrics.visible_columns = static_cast<std::size_t>(
      std::max(8.0f, (rect.w - metrics.gutter_width - 28.0f) / char_width));
  return metrics;
}

void EditorViewRenderer::Render(SDL_Renderer* renderer,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                TextViewport& viewport,
                                const SDL_FRect& rect,
                                bool draw_caret,
                                std::string_view search_query,
                                const std::optional<SelectionRange>& active_search_match,
                                const std::optional<EditorBlameOverlay>& blame_overlay) const {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("EditorViewRenderer::Render");
  SDL_SetRenderDrawColor(renderer, theme.editor_background.r, theme.editor_background.g,
                         theme.editor_background.b, theme.editor_background.a);
  SDL_RenderFillRect(renderer, &rect);

  if (viewport.is_placeholder()) {
    DrawPlaceholderView(renderer, text_renderer, theme, rect);
    viewport.SetViewportSize(1, 1);
    return;
  }

  const EditorViewMetrics metrics = ComputeMetrics(text_renderer, viewport, rect);
  const SDL_FRect gutter = SDL_FRect{rect.x, rect.y, metrics.gutter_width, rect.h};
  SDL_SetRenderDrawColor(renderer, theme.gutter_background.r, theme.gutter_background.g,
                         theme.gutter_background.b, theme.gutter_background.a);
  SDL_RenderFillRect(renderer, &gutter);
  const SDL_FRect gutter_divider =
      SDL_FRect{gutter.x + gutter.w - 1.0f, gutter.y, 1.0f, gutter.h};
  SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
  SDL_RenderFillRect(renderer, &gutter_divider);

  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const auto& lines = viewport.lines();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t cursor_line = viewport.cursor_line();
  const auto selection = viewport.selection_range();
  const std::string lowered_search_query = ToLower(search_query);
  std::size_t blame_index = 0;

  for (std::size_t row = 0; row < metrics.visible_rows; ++row) {
    const std::size_t line_index = scroll_line + row;
    if (line_index >= lines.size()) {
      break;
    }

    const float y = metrics.first_line_y + static_cast<float>(row) * metrics.line_height;
    const bool selected = line_index == cursor_line;
    const bool active_search_line =
        active_search_match.has_value() &&
        line_index >= active_search_match->start.line &&
        line_index <= active_search_match->end.line;
    const SDL_Color row_background = selected ? theme.row_highlight : theme.editor_background;
    DecoratedTextRow row_desc;
    if (selected) {
      row_desc.fills.push_back(DecoratedTextFill{
          .rect = SDL_FRect{rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, metrics.line_height},
          .color = theme.row_highlight,
      });
    }

    if (!lowered_search_query.empty()) {
      const std::string lowered_line = ToLower(lines[line_index]);
      std::size_t match_offset = lowered_line.find(lowered_search_query);
      while (match_offset != std::string::npos) {
        const std::size_t start_visual =
            TextLayout::VisualColumnForTextColumn(lines[line_index], match_offset, viewport.tab_size());
        const std::size_t end_visual = TextLayout::VisualColumnForTextColumn(
            lines[line_index], match_offset + lowered_search_query.size(), viewport.tab_size());
        const std::size_t visible_start = std::max(start_visual, viewport.horizontal_scroll());
        const std::size_t visible_end = std::min(end_visual,
                                                 viewport.horizontal_scroll() + viewport.visible_columns());
        if (visible_end > visible_start) {
          const bool is_active_match =
              active_search_line &&
              match_offset == active_search_match->start.column &&
              line_index == active_search_match->start.line;
          row_desc.fills.push_back(DecoratedTextFill{
              .rect =
                  SDL_FRect{
                      metrics.text_x +
                          static_cast<float>(visible_start - viewport.horizontal_scroll()) *
                              text_renderer.CharWidth(),
                      y - 1.0f,
                      static_cast<float>(visible_end - visible_start) * text_renderer.CharWidth(),
                      metrics.line_height,
                  },
              .color = is_active_match ? theme.search_match_active : theme.search_match,
          });
        }
        match_offset = lowered_line.find(lowered_search_query, match_offset + 1);
      }
    }

    if (selection.has_value() &&
        line_index >= selection->start.line &&
        line_index <= selection->end.line) {
      const std::size_t line_start =
          line_index == selection->start.line ? selection->start.column : 0;
      const std::size_t line_end =
          line_index == selection->end.line ? selection->end.column : lines[line_index].size();
      const std::size_t start_visual =
          TextLayout::VisualColumnForTextColumn(lines[line_index], line_start, viewport.tab_size());
      const std::size_t end_visual =
          TextLayout::VisualColumnForTextColumn(lines[line_index], line_end, viewport.tab_size());
      const std::size_t visible_start = std::max(start_visual, viewport.horizontal_scroll());
      const std::size_t visible_end = std::min(end_visual,
                                               viewport.horizontal_scroll() + viewport.visible_columns());
      if (visible_end > visible_start) {
        row_desc.fills.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    metrics.text_x +
                        static_cast<float>(visible_start - viewport.horizontal_scroll()) *
                            text_renderer.CharWidth(),
                    y - 1.0f,
                    static_cast<float>(visible_end - visible_start) * text_renderer.CharWidth(),
                    metrics.line_height,
                },
            .color = theme.selection_fill,
        });
      }
    }

    const auto layout = viewport.VisibleLineLayout(line_index);
    const std::vector<SyntaxTokenKind>& token_kinds =
        viewport.HighlightedLineTokens(line_index);
    AppendLayoutSyntaxTextRuns(row_desc, text_renderer, theme, metrics.text_x, y, layout,
                               selected ? theme.text_primary : theme.text_secondary,
                               token_kinds);
    kDecoratedRowRenderer.RenderRow(renderer, text_renderer, row_desc);
    text_renderer.DrawStringOn(renderer, gutter.x + 10.0f, y,
                               selected ? theme.current_line_number : theme.line_number,
                               selected ? theme.row_highlight : theme.gutter_background,
                               std::to_string(line_index + 1));

    if (draw_caret && selected && layout.caret_visible) {
      const float caret_x = metrics.text_x +
                            static_cast<float>(layout.caret_column) * text_renderer.CharWidth();
      const SDL_FRect caret =
          SDL_FRect{caret_x, y - 1.0f, 1.5f, metrics.line_height};
      SDL_SetRenderDrawColor(renderer, theme.cursor.r, theme.cursor.g, theme.cursor.b,
                             theme.cursor.a);
      SDL_RenderFillRect(renderer, &caret);
    }

    if (blame_overlay.has_value() && blame_overlay->visible) {
      while (blame_index < blame_overlay->lines.size() &&
             blame_overlay->lines[blame_index].line_index < line_index) {
        ++blame_index;
      }
      if (blame_index < blame_overlay->lines.size() &&
          blame_overlay->lines[blame_index].line_index == line_index) {
        text_renderer.DrawStringOn(renderer, blame_overlay->lines[blame_index].rect.x,
                                   blame_overlay->lines[blame_index].rect.y, theme.text_disabled,
                                   row_background,
                                   blame_overlay->lines[blame_index].text);
      }
    }
  }
}

}  // namespace microide::editor
