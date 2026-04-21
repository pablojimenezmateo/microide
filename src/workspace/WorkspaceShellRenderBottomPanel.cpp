#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

using namespace detail;

namespace {

std::string MessageRoleLabel(MessageRole role) {
  switch (role) {
    case MessageRole::User:
      return "You";
    case MessageRole::Assistant:
      return "Assistant";
    case MessageRole::System:
    default:
      return "System";
  }
}

}  // namespace

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
  const bool terminal_panel = BottomPanelShowsTerminal();
  const bool output_panel = BottomPanelShowsOutput();
  const bool chat_panel = BottomPanelShowsChat();

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
          tab.index < context_.current_project_state.terminal_tabs.size()
              ? context_.current_project_state.terminal_tabs[tab.index].get()
              : nullptr;
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
    std::string header_label = "Command";
    if (output_panel) {
      header_label = "Output";
      for (const auto& channel : output_channels_.Channels()) {
        if (channel.id == context_.current_project_state.panel.output.channel_id) {
          header_label = channel.label.empty() ? channel.id : channel.label;
          break;
        }
      }
    } else if (chat_panel) {
      header_label = "Chat";
      if (const Conversation* conversation = conversation_registry_.GetConversation(
              context_.current_project_state.panel.chat.conversation_id);
          conversation != nullptr && !conversation->title.empty()) {
        header_label = conversation->title;
      }
    }
    DrawVCenteredTextOn(text_renderer_, renderer, panel_header, 12.0f, theme_.chrome_text,
                        theme_.chrome_background, header_label);
  }

  const std::vector<std::string>* output_entries =
      output_panel ? OutputChannelEntries(context_.current_project_state.panel.output.channel_id)
                   : nullptr;
  const Conversation* conversation =
      chat_panel
          ? conversation_registry_.GetConversation(
                context_.current_project_state.panel.chat.conversation_id)
          : nullptr;
  const std::size_t panel_line_count =
      terminal_panel ? terminal_line_count
                     : output_panel ? (output_entries != nullptr ? output_entries->size() : 0)
                                    : chat_panel ? (conversation != nullptr
                                                        ? conversation->messages.size()
                                                        : 0)
                                                 : 0;

  const BottomPanelLogLayout panel_layout =
      ComputeBottomPanelLogLayout(layout, panel_line_count);
  SetBottomPanelScrollRow(panel_layout.scroll.vertical_scroll, panel_line_count,
                          panel_layout.scroll.visible_rows);

  const std::size_t first_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const std::vector<terminal::TerminalLine> terminal_lines =
      terminal_panel && ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLineRange(
                first_row,
                static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)))
          : std::vector<terminal::TerminalLine>{};

  for (int row = 0; row < panel_layout.scroll.visible_rows; ++row) {
    const int index = panel_layout.scroll.vertical_scroll + row;
    if (index >= static_cast<int>(panel_line_count)) {
      break;
    }
    const float line_y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height;
    if (terminal_panel) {
      draw_terminal_line(panel_layout.text_x, line_y, panel_layout.text_width,
                         terminal_lines[static_cast<std::size_t>(index) - first_row],
                         static_cast<std::size_t>(index));
      continue;
    }
    if (output_panel && output_entries != nullptr) {
      DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_secondary,
                 theme_.surface_background,
                 text_renderer_.TruncateToWidth((*output_entries)[static_cast<std::size_t>(index)],
                                                panel_layout.text_width));
      continue;
    }
    if (chat_panel && conversation != nullptr) {
      const Message& message = conversation->messages[static_cast<std::size_t>(index)];
      const SDL_Color color =
          message.role == MessageRole::Assistant
              ? theme_.text_primary
              : message.role == MessageRole::User ? theme_.accent : theme_.text_muted;
      const std::string line =
          MessageRoleLabel(message.role) + ": " + CollapseWhitespace(message.content);
      DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, color,
                 theme_.surface_background,
                 text_renderer_.TruncateToWidth(line, panel_layout.text_width));
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
          (context_.current_project_state.surface.focus != FocusTarget::Panel ||
           CaretVisibleNow())) {
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
          if (cursor_row >= first_row &&
              cursor_row - first_row < terminal_lines.size()) {
            const auto& line = terminal_lines[cursor_row - first_row];
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

  if (context_.current_project_state.panel.command_mode || chat_panel) {
    const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
    DrawFilledRect(renderer, command_area, theme_.surface_raised);
    DrawFilledRect(renderer,
                   MakeRect(command_area.x, command_area.y, command_area.w,
                            kWorkspaceDividerThickness),
                   theme_.border);

    const float status_y = command_area.y + kWorkspaceBottomPanelCommandTopPadding;
    const std::string status_text =
        context_.current_project_state.panel.command_mode
            ? CommandPromptCoordinator::PromptStatusText(
                  context_.current_project_state.panel.command)
            : !context_.current_project_state.panel.chat.status_text.empty()
                ? context_.current_project_state.panel.chat.status_text
                : "Enter sends the current prompt";
    DrawTextOn(text_renderer_, renderer, command_area.x + 12.0f, status_y, theme_.text_muted,
               theme_.surface_raised, TruncateLabel(status_text, command_area.w - 24.0f));

    const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
    DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
    DrawVCenteredTextOn(
        text_renderer_, renderer, prompt_rect, 6.0f, theme_.chrome_active_text,
        theme_.chrome_active,
        "> " +
            (context_.current_project_state.panel.command_mode
                 ? context_.current_project_state.panel.command.input
                 : context_.current_project_state.panel.chat.composer));
  }

  if (panel_layout.scroll.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, panel_layout.scroll.vertical_scrollbar->track,
                  panel_layout.scroll.vertical_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar);
  }
}

}  // namespace microide::workspace
