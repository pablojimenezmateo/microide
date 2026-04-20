#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderBottomPanelSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout,
                                              std::size_t terminal_line_count) {
  if (!BottomPanelVisible()) {
    return;
  }

  util::PerformanceTrace::Scope bottom_panel_scope("WorkspaceShell::RenderBottomPanel");
  const SDL_FRect panel_header =
      MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
               kWorkspaceBottomPanelHeaderHeight);
  const bool terminal_panel = ActiveTerminalTab() != nullptr;

  const auto draw_tab_close_button = [&](const SDL_FRect& rect,
                                         SDL_Color color,
                                         SDL_Color hover_color) {
    const bool hovered =
        last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
    DrawCloseGlyph(renderer, rect, hovered ? hover_color : color);
  };

  const auto resolve_terminal_colors = [&](const terminal::TerminalStyle& style, bool selected) {
    SDL_Color foreground = style.foreground.value_or(theme_.text_muted);
    SDL_Color background = style.background.value_or(theme_.surface_background);
    if (style.inverse) {
      std::swap(foreground, background);
    }
    if (selected) {
      foreground = theme_.text_primary;
      background = theme_.row_highlight;
    }
    return std::pair{foreground, background};
  };
  const auto draw_terminal_line = [&](float x,
                                      float y,
                                      float width,
                                      const terminal::TerminalLine& line,
                                      std::size_t row_index) {
    if (width <= 0.0f || line.cells.empty()) {
      return;
    }

    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const std::size_t visible_columns =
        std::min(line.cells.size(), std::max<std::size_t>(
                                      1, static_cast<std::size_t>(std::floor(width / char_width))));
    if (visible_columns == 0) {
      return;
    }

    for (std::size_t column = 0; column < visible_columns;) {
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      const SDL_Color background = resolve_terminal_colors(cell.style, selected).second;

      std::size_t run_end = column + 1;
      while (run_end < visible_columns) {
        const auto& next_cell = line.cells[run_end];
        const bool next_selected = TerminalCellSelected(row_index, run_end);
        const SDL_Color next_background =
            resolve_terminal_colors(next_cell.style, next_selected).second;
        if (next_background.r != background.r || next_background.g != background.g ||
            next_background.b != background.b || next_background.a != background.a) {
          break;
        }
        ++run_end;
      }

      const float run_x = x + static_cast<float>(column) * char_width;
      DrawFilledRect(renderer,
                     MakeRect(run_x, y - 1.0f,
                              static_cast<float>(run_end - column) * char_width,
                              text_renderer_.LineHeight()),
                     background);
      column = run_end;
    }

    for (std::size_t column = 0; column < visible_columns; ++column) {
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      const auto [foreground, background] = resolve_terminal_colors(cell.style, selected);
      (void)background;
      const float cell_x = x + static_cast<float>(column) * char_width;
      const std::string_view display_text = cell.DisplayText();
      if (display_text.empty() || display_text == " ") {
        continue;
      }
      text_renderer_.DrawString(renderer, cell_x, y, foreground, display_text);
    }
  };

  if (terminal_panel) {
    for (const VisibleStripTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
      const auto* terminal_tab =
          tab.index < terminal_tabs_.size() ? terminal_tabs_[tab.index].get() : nullptr;
      if (terminal_tab == nullptr) {
        continue;
      }

      const SDL_Color background = tab.active ? theme_.chrome_active : theme_.surface_raised;
      const SDL_Color foreground =
          tab.active ? theme_.chrome_active_text : theme_.surface_text;
      DrawFilledRect(renderer, tab.rect, background);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      DrawVCenteredTextOn(text_renderer_, renderer, tab.rect, 8.0f, foreground, background,
                          TruncateLabel(tab.display_title, tab.rect.w - 40.0f));
      draw_tab_close_button(tab.close_rect, foreground, theme_.chrome_active_text);
    }
    const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
    DrawFilledRect(renderer, new_tab_rect, theme_.surface_raised);
    DrawRect(renderer, new_tab_rect, theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, new_tab_rect, theme_.surface_text,
                       theme_.surface_raised, "+");
  } else {
    DrawVCenteredTextOn(text_renderer_, renderer, panel_header, 12.0f, theme_.chrome_text,
                        theme_.chrome_background, "Command");
  }

  const BottomPanelLogLayout panel_layout =
      ComputeBottomPanelLogLayout(layout, terminal_panel ? terminal_line_count : 0);
  SetBottomPanelScrollRow(panel_layout.scroll.vertical_scroll,
                          terminal_panel ? terminal_line_count : 0,
                          panel_layout.scroll.visible_rows);
  const std::size_t first_terminal_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const std::vector<terminal::TerminalLine> terminal_lines =
      terminal_panel && ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLineRange(
                first_terminal_row,
                static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)))
          : std::vector<terminal::TerminalLine>{};
  for (int row = 0; row < panel_layout.scroll.visible_rows; ++row) {
    const int index = panel_layout.scroll.vertical_scroll + row;
    if (index >= static_cast<int>(terminal_line_count)) {
      break;
    }
    const float line_y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height;
    if (terminal_panel) {
      draw_terminal_line(panel_layout.text_x, line_y, panel_layout.text_width,
                         terminal_lines[static_cast<std::size_t>(index) - first_terminal_row],
                         static_cast<std::size_t>(index));
    }
  }

  if (terminal_panel) {
    if (auto* active_terminal = ActiveTerminalTab();
        active_terminal != nullptr && active_terminal->session.cursor_visible()) {
      const std::size_t cursor_row = active_terminal->session.cursor_row();
      const std::size_t cursor_column = active_terminal->session.cursor_column();
      if (cursor_row >= static_cast<std::size_t>(panel_layout.scroll.vertical_scroll) &&
          cursor_row <
              static_cast<std::size_t>(panel_layout.scroll.vertical_scroll +
                                       panel_layout.scroll.visible_rows) &&
          (surface_.focus != FocusTarget::Panel || CaretVisibleNow())) {
        const float char_width = std::max(1.0f, text_renderer_.CharWidth());
        const float cursor_x = panel_layout.text_x + static_cast<float>(cursor_column) * char_width;
        const float cursor_y =
            panel_layout.text_y +
            static_cast<float>(cursor_row -
                               static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
                panel_layout.line_height;
        if (cursor_x <= panel_layout.content_rect.x + panel_layout.content_rect.w - char_width) {
          DrawFilledRect(renderer,
                         MakeRect(cursor_x, cursor_y - 1.0f, char_width, panel_layout.line_height),
                         theme_.cursor);
          if (cursor_row >= first_terminal_row &&
              cursor_row - first_terminal_row < terminal_lines.size()) {
            const auto& line = terminal_lines[cursor_row - first_terminal_row];
            if (cursor_column < line.cells.size()) {
              const auto& cell = line.cells[cursor_column];
              const auto display_text = cell.DisplayText();
              if (!display_text.empty()) {
                const SDL_Color cursor_foreground =
                    resolve_terminal_colors(cell.style, false).second;
                text_renderer_.DrawString(renderer, cursor_x, cursor_y, cursor_foreground,
                                          std::string(display_text));
              }
            }
          }
        }
      }
    }
  }

  if (panel_state_.command_mode) {
    const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
    DrawFilledRect(renderer, command_area, theme_.surface_raised);
    DrawFilledRect(renderer,
                   MakeRect(command_area.x, command_area.y, command_area.w,
                            kWorkspaceDividerThickness),
                   theme_.border);

    const float status_y = command_area.y + kWorkspaceBottomPanelCommandTopPadding;
    DrawTextOn(text_renderer_, renderer, command_area.x + 12.0f, status_y, theme_.text_muted,
               theme_.surface_raised,
               TruncateLabel(CommandPromptCoordinator::PromptStatusText(*this),
                             command_area.w - 24.0f));

    const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
    DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
    DrawVCenteredTextOn(text_renderer_, renderer, prompt_rect, 6.0f,
                        theme_.chrome_active_text, theme_.chrome_active,
                        "> " + command_.input);
  }

  if (panel_layout.scroll.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, panel_layout.scroll.vertical_scrollbar->track,
                  panel_layout.scroll.vertical_scrollbar->thumb,
                 interaction_state_.drag_target == DragTarget::BottomPanelScrollbar);
  }
}

}  // namespace microide::workspace
