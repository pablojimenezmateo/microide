#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

using namespace detail;

namespace {

const std::vector<terminal::TerminalLine>& EmptyTerminalLines() {
  static const std::vector<terminal::TerminalLine> empty_lines;
  return empty_lines;
}

}  // namespace

void WorkspaceShell::RenderBottomPanelSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout,
                                              std::size_t terminal_line_count) {
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  const TextInputSurfaceViewModel& text_input_vm = *prepare_cached_text_input_vm_;
  if (!panel_vm.command_mode && panel_vm.content == PanelContentKind::None) {
    return;
  }

  util::PerformanceTrace::Scope bottom_panel_scope("WorkspaceShell::RenderBottomPanel");
  const SDL_FRect panel_header =
      MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
               kWorkspaceBottomPanelHeaderHeight);
  const bool terminal_panel = panel_vm.content == PanelContentKind::Terminal;
  const bool output_panel = panel_vm.content == PanelContentKind::Output;
  const std::vector<VisibleStripTab> visible_panel_tabs =
      tab_strip_service_.ComputeVisibleBottomPanelTabs(
          *panel_vm.project_state, panel_header, layout_mode_service_.CurrentMode(),
          [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
          output_channels_.Channels());

  const auto draw_tab_close_button = [&](const SDL_FRect& rect,
                                         SDL_Color color,
                                         SDL_Color hover_color) {
    DrawHoverableCloseGlyph(renderer, rect,
                            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_),
                            color, hover_color);
  };
  const StripTabPalette panel_tab_palette{
      .active_fill = render::BlendColors(theme_.chrome_active, theme_.surface_background, 0.42f),
      .inactive_fill = render::BlendColors(theme_.surface_raised, theme_.surface_background, 0.66f),
      .active_text = theme_.chrome_active_text,
      .inactive_text = theme_.text_secondary,
      .active_glyph = theme_.chrome_active_text,
      .inactive_glyph = theme_.text_secondary,
  };

  const auto resolve_terminal_colors = [&](const terminal::TerminalStyle& style, bool selected) {
    SDL_Color foreground = style.foreground.value_or(theme_.text_primary);
    SDL_Color background = style.background.value_or(theme_.surface_background);
    if (style.inverse()) {
      std::swap(foreground, background);
    }
    if (style.dim()) {
      foreground = render::BlendColors(foreground, background, 0.40f);
    }
    if (style.hidden()) {
      foreground = background;
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

    terminal_foreground_runs_scratch_.clear();
    terminal_foreground_runs_blob_.clear();

    const auto is_wide_lead = [&](std::size_t column) {
      return column + 1 < line.cells.size() && line.cells[column + 1].style.wide_trailing();
    };

    for (std::size_t column = 0; column < visible_columns;) {
      const auto& cell = line.cells[column];
      // Trailing spacer of a double-width glyph: painted by its lead cell, so
      // never emit a glyph (a blank here would erase the right half).
      if (cell.style.wide_trailing()) {
        ++column;
        continue;
      }
      const bool selected = TerminalCellSelected(row_index, column);
      const SDL_Color foreground = resolve_terminal_colors(cell.style, selected).first;

      std::size_t run_end = column + 1;
      // A wide glyph is positioned on its own grid column, so it terminates the
      // run; the following spacer column is skipped on the next iteration.
      if (!is_wide_lead(column)) {
        while (run_end < visible_columns) {
          const auto& next_cell = line.cells[run_end];
          if (next_cell.style.wide_trailing() || is_wide_lead(run_end)) {
            break;
          }
          const bool next_selected = TerminalCellSelected(row_index, run_end);
          const SDL_Color next_foreground =
              resolve_terminal_colors(next_cell.style, next_selected).first;
          if (next_foreground.r != foreground.r || next_foreground.g != foreground.g ||
              next_foreground.b != foreground.b || next_foreground.a != foreground.a) {
            break;
          }
          ++run_end;
        }
      }

      const std::size_t blob_start = terminal_foreground_runs_blob_.size();
      bool has_non_space = false;
      for (std::size_t index = column; index < run_end; ++index) {
        const std::string_view display_text = line.cells[index].DisplayText();
        if (display_text.empty()) {
          terminal_foreground_runs_blob_.push_back(' ');
          continue;
        }
        if (display_text != " ") {
          has_non_space = true;
        }
        terminal_foreground_runs_blob_.append(display_text);
      }
      TerminalForegroundRunScratch run{};
      run.start_column = column;
      run.end_column_exclusive = run_end;
      run.foreground = foreground;
      run.blob_offset = blob_start;
      run.blob_length = terminal_foreground_runs_blob_.size() - blob_start;
      run.has_visible_text = has_non_space;
      terminal_foreground_runs_scratch_.push_back(run);
      column = run_end;
    }

    for (const TerminalForegroundRunScratch& run : terminal_foreground_runs_scratch_) {
      if (!run.has_visible_text) {
        continue;
      }
      const float run_x = x + static_cast<float>(run.start_column) * char_width;
      const std::string_view run_text(terminal_foreground_runs_blob_.data() + run.blob_offset,
                                      run.blob_length);
      text_renderer_.DrawString(renderer, run_x, y, run.foreground, run_text);
    }

    // Underline / double-underline / strikethrough decorations. Run-length
    // coalesced over (foreground color, decoration bits) so a styled span emits
    // a single filled rect rather than one per cell. Wide trailing spacers carry
    // the lead cell's style, so the decoration spans both columns of a wide glyph.
    constexpr std::uint16_t kDecorationMask = terminal::cell_attr::kUnderline |
                                              terminal::cell_attr::kDoubleUnderline |
                                              terminal::cell_attr::kStrikethrough;
    const float line_height = text_renderer_.LineHeight();
    const float thickness = std::max(1.0f, std::round(line_height / 14.0f));
    for (std::size_t column = 0; column < visible_columns;) {
      const auto& style = line.cells[column].style;
      const std::uint16_t decoration = static_cast<std::uint16_t>(style.attrs & kDecorationMask);
      if (decoration == 0) {
        ++column;
        continue;
      }
      const bool selected = TerminalCellSelected(row_index, column);
      const SDL_Color color = resolve_terminal_colors(style, selected).first;
      std::size_t run_end = column + 1;
      while (run_end < visible_columns) {
        const auto& next_style = line.cells[run_end].style;
        if (static_cast<std::uint16_t>(next_style.attrs & kDecorationMask) != decoration) {
          break;
        }
        const SDL_Color next_color =
            resolve_terminal_colors(next_style, TerminalCellSelected(row_index, run_end)).first;
        if (next_color.r != color.r || next_color.g != color.g || next_color.b != color.b ||
            next_color.a != color.a) {
          break;
        }
        ++run_end;
      }

      const float run_x = x + static_cast<float>(column) * char_width;
      const float run_w = static_cast<float>(run_end - column) * char_width;
      if (decoration & terminal::cell_attr::kStrikethrough) {
        DrawFilledRect(renderer, MakeRect(run_x, y + std::round(line_height * 0.45f), run_w, thickness),
                       color);
      }
      if (decoration & terminal::cell_attr::kUnderline) {
        DrawFilledRect(renderer, MakeRect(run_x, y + line_height - thickness - 1.0f, run_w, thickness),
                       color);
      }
      if (decoration & terminal::cell_attr::kDoubleUnderline) {
        DrawFilledRect(renderer, MakeRect(run_x, y + line_height - thickness * 3.0f - 1.0f, run_w,
                                          thickness),
                       color);
        DrawFilledRect(renderer, MakeRect(run_x, y + line_height - thickness - 1.0f, run_w, thickness),
                       color);
      }
      column = run_end;
    }
  };

  if (!visible_panel_tabs.empty()) {
    for (const VisibleStripTab& tab : visible_panel_tabs) {
      DrawStripTab(text_renderer_, renderer, theme_, tab.rect, tab.display_title, tab.badge_text,
                   tab.badge_color, tab.show_badge, tab.active,
                   StripTabStyle{
                       .text_left_padding = 8.0f,
                       .close_right_reserve = 40.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   panel_tab_palette);
      draw_tab_close_button(tab.close_rect,
                            tab.active ? panel_tab_palette.active_glyph
                                       : panel_tab_palette.inactive_glyph,
                            panel_tab_palette.active_text);
    }
    const SDL_FRect new_tab_rect = tab_strip_service_.BottomPanelTerminalNewTabRect(
        layout_mode_service_.CurrentMode(), panel_header);
    DrawButtonCentered(
        text_renderer_, renderer, theme_, new_tab_rect, "", ButtonTone::Neutral,
        ButtonVisualState{
            .enabled = true,
            .hovered = last_mouse_position_valid_ && Contains(new_tab_rect, last_mouse_x_, last_mouse_y_),
            .active = false,
        });
    DrawPlusGlyph(renderer, new_tab_rect,
                  last_mouse_position_valid_ && Contains(new_tab_rect, last_mouse_x_, last_mouse_y_)
                      ? theme_.text_primary
                      : theme_.text_secondary);
    const auto panel_overflow = tab_strip_service_.ComputeBottomPanelTabOverflowControls(
        *panel_vm.project_state, panel_header, layout_mode_service_.CurrentMode(),
        visible_panel_tabs, output_channels_.Channels());
    DrawTabStripOverflowButton(
        text_renderer_, renderer, theme_, panel_overflow.left_button, /*point_right=*/false,
        panel_overflow.hidden_left,
        last_mouse_position_valid_ &&
            Contains(panel_overflow.left_button, last_mouse_x_, last_mouse_y_));
    DrawTabStripOverflowButton(
        text_renderer_, renderer, theme_, panel_overflow.right_button, /*point_right=*/true,
        panel_overflow.hidden_right,
        last_mouse_position_valid_ &&
            Contains(panel_overflow.right_button, last_mouse_x_, last_mouse_y_));
    if (panel_vm.tab_drag.active) {
      DrawTabDragFeedback(text_renderer_, renderer, theme_, panel_header, visible_panel_tabs,
                          panel_vm.tab_drag.source_index, panel_vm.tab_drag.target_slot,
                          panel_vm.tab_drag.pointer_x, panel_vm.tab_drag.grab_offset_x,
                          StripTabStyle{
                              .text_left_padding = 8.0f,
                              .close_right_reserve = 40.0f,
                              .accent_edge = StripAccentEdge::Top,
                          },
                          panel_tab_palette);
    }
  } else {
    // Header label is one of: "Command" (default), "Output" (no channel match), or the channel's
    // label/id. Hold via string_view so the constant case allocates nothing per frame.
    std::string_view header_label = "Command";
    if (output_panel) {
      header_label = "Output";
      for (const auto& channel : output_channels_.Channels()) {
        if (channel.id == panel_vm.output_channel_id) {
          header_label = channel.label.empty() ? std::string_view(channel.id)
                                                : std::string_view(channel.label);
          break;
        }
      }
    }
    DrawVCenteredTextOn(text_renderer_, renderer, panel_header, 12.0f, theme_.chrome_text,
                        theme_.chrome_background, header_label);
  }

  if (const editor::TextViewport* viewport = ActiveEditableViewport();
      viewport != nullptr && !viewport->path().empty()) {
    const std::string lsp_status_text = ActiveLspStatusText(/*ensure_started=*/false);
    const float status_width = text_renderer_.MeasureWidth(lsp_status_text);
    const float status_right = visible_panel_tabs.empty()
                                   ? panel_header.x + panel_header.w - 12.0f
                                   : tab_strip_service_.BottomPanelTerminalNewTabRect(
                                         layout_mode_service_.CurrentMode(), panel_header)
                                         .x -
                                         12.0f;
    const float status_x = status_right - status_width;
    if (status_x > panel_header.x + std::min(panel_header.w * 0.5f, 220.0f)) {
      DrawVCenteredTextOn(text_renderer_, renderer,
                          MakeRect(status_x, panel_header.y,
                                   std::max(0.0f, status_right - status_x), panel_header.h),
                          0.0f, theme_.text_muted, theme_.chrome_background, lsp_status_text);
    }
  }

  const std::vector<std::string>* output_entries =
      output_panel ? OutputChannelEntries(panel_vm.output_channel_id)
                   : nullptr;
  std::optional<std::filesystem::path> current_reference_path;
  const std::size_t panel_line_count =
      terminal_panel ? terminal_line_count
                     : output_panel ? (output_entries != nullptr ? output_entries->size() : 0)
                                    : 0;

  const BottomPanelLogLayout panel_layout =
      ComputeBottomPanelLogLayout(layout, panel_line_count);
  SetBottomPanelScrollRow(panel_layout.scroll.vertical_scroll, panel_line_count,
                          panel_layout.scroll.visible_rows);

  const std::size_t first_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const std::vector<terminal::TerminalLine>* terminal_lines = &EmptyTerminalLines();
  if (terminal_panel) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      const std::size_t visible_rows =
          static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows));
      const bool same_visible_range =
          terminal_tab->visible_lines_first_row == first_row &&
          terminal_tab->visible_lines_max_rows == visible_rows;
      const std::uint64_t previous_generation =
          same_visible_range ? terminal_tab->visible_lines_snapshot.generation : 0;
      terminal_tab->session.SnapshotLineRangeIfChanged(
          first_row, visible_rows, previous_generation, &terminal_tab->visible_lines_snapshot);
      terminal_tab->visible_lines_first_row = first_row;
      terminal_tab->visible_lines_max_rows = visible_rows;
      terminal_lines = &terminal_tab->visible_lines_snapshot.lines;
    }
  }

  for (int row = 0; row < panel_layout.scroll.visible_rows; ++row) {
    const int index = panel_layout.scroll.vertical_scroll + row;
    if (index >= static_cast<int>(panel_line_count)) {
      break;
    }
    const float line_y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height;
    if (terminal_panel) {
      // panel_line_count comes from `session.LineCount()` sampled outside the
      // session mutex, while `terminal_lines` is the locked snapshot. They can
      // disagree when scrollback is trimmed between the two calls — the
      // snapshot is the source of truth for what to draw. Guard against that
      // skew before indexing (round-4 Finding 3 follow-on).
      const std::size_t snapshot_index = static_cast<std::size_t>(index) - first_row;
      if (snapshot_index >= terminal_lines->size()) {
        break;
      }
      draw_terminal_line(panel_layout.text_x, line_y, panel_layout.text_width,
                         (*terminal_lines)[snapshot_index],
                         static_cast<std::size_t>(index));
      continue;
    }
    if (output_panel && output_entries != nullptr) {
      const std::size_t output_index = static_cast<std::size_t>(index);
      const std::string& output_line = (*output_entries)[output_index];
      if (const auto* parsed = output_channels_.ParsedEntryAt(
              panel_vm.output_channel_id, output_index);
          parsed != nullptr) {
        if (parsed->kind == WorkspaceOutputChannels::ParsedEntry::Kind::ReferencePath) {
          std::filesystem::path resolved_path = parsed->reference_path;
          if (resolved_path.is_relative() && !panel_vm.project_root.empty()) {
            resolved_path = panel_vm.project_root / resolved_path;
          }
          current_reference_path = resolved_path.lexically_normal();
        } else if (parsed->kind == WorkspaceOutputChannels::ParsedEntry::Kind::ContextSnippet) {
          std::filesystem::path resolved_path = parsed->reference_path;
          if (resolved_path.is_relative() && !panel_vm.project_root.empty()) {
            resolved_path = panel_vm.project_root / resolved_path;
          }
          current_reference_path = resolved_path.lexically_normal();

          const std::string visible_prefix =
              text_renderer_.TruncateToWidth(parsed->prefix, panel_layout.text_width);
          DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_secondary,
                     theme_.surface_background, visible_prefix);

          if (visible_prefix.size() == parsed->prefix.size()) {
            const float prefix_width = text_renderer_.MeasureWidth(parsed->prefix);
            const float remaining_width = panel_layout.text_width - prefix_width;
            if (remaining_width > 0.0f && !parsed->code.empty()) {
              const std::string visible_code =
                  text_renderer_.TruncateToWidth(parsed->code, remaining_width);
              const editor::HighlightedLine* highlighted =
                  output_channels_.HighlightedContextSnippet(
                      panel_vm.output_channel_id, output_index,
                      current_reference_path.value_or(std::filesystem::path{}));
              if (highlighted != nullptr && highlighted->tokens.size() >= visible_code.size()) {
                float run_x = panel_layout.text_x + prefix_width;
                for (std::size_t start = 0; start < visible_code.size();) {
                  const editor::SyntaxTokenKind kind = highlighted->tokens[start];
                  std::size_t end = start + 1;
                  while (end < visible_code.size() && highlighted->tokens[end] == kind) {
                    ++end;
                  }
                  const std::string_view segment(visible_code.data() + start, end - start);
                  const SDL_Color color =
                      editor::SyntaxTokenColor(theme_, kind, theme_.text_secondary);
                  text_renderer_.DrawString(renderer, run_x, line_y, color, segment);
                  run_x += text_renderer_.MeasureWidth(segment);
                  start = end;
                }
              } else {
                DrawTextOn(text_renderer_, renderer, panel_layout.text_x + prefix_width, line_y,
                           theme_.text_secondary, theme_.surface_background, visible_code);
              }
            }
          }
          continue;
        } else if (output_line.empty()) {
          current_reference_path.reset();
        }
      }
      DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_secondary,
                 theme_.surface_background,
                 text_renderer_.TruncateToWidth(output_line, panel_layout.text_width));
      continue;
    }
  }

  if (terminal_panel) {
    if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr) {
      const terminal::TerminalCursorSnapshot cursor = active_terminal->session.CursorSnapshot();
      if (cursor.visible &&
          cursor.row >= static_cast<std::size_t>(panel_layout.scroll.vertical_scroll) &&
          cursor.row <
              static_cast<std::size_t>(panel_layout.scroll.vertical_scroll +
                                       panel_layout.scroll.visible_rows) &&
          (panel_vm.focus != FocusTarget::Panel ||
           CaretVisibleNow())) {
        const float char_width = std::max(1.0f, text_renderer_.CharWidth());
        const float cursor_x = panel_layout.text_x + static_cast<float>(cursor.column) * char_width;
        const float cursor_y =
            panel_layout.text_y +
            static_cast<float>(cursor.row -
                               static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
                panel_layout.line_height;
        if (cursor_x <= panel_layout.content_rect.x + panel_layout.content_rect.w - char_width) {
          DrawFilledRect(renderer,
                         MakeRect(cursor_x, cursor_y - 1.0f, char_width, panel_layout.line_height),
                         theme_.cursor);
          if (cursor.row >= first_row &&
              cursor.row - first_row < terminal_lines->size()) {
            const auto& line = (*terminal_lines)[cursor.row - first_row];
            if (cursor.column < line.cells.size()) {
              const auto& cell = line.cells[cursor.column];
              const auto display_text = cell.DisplayText();
              if (!display_text.empty()) {
                const SDL_Color cursor_foreground =
                    resolve_terminal_colors(cell.style, false).second;
                // DrawString takes std::string_view; pass the view directly without copying.
                text_renderer_.DrawString(renderer, cursor_x, cursor_y, cursor_foreground,
                                          display_text);
              }
            }
          }
        }
      }
    }
  }

  if (panel_vm.command_mode) {
    const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
    DrawFilledRect(renderer, command_area, theme_.surface_raised);
    DrawFilledRect(renderer,
                   MakeRect(command_area.x, command_area.y, command_area.w,
                            kWorkspaceDividerThickness),
                   theme_.border);

    const float status_y = command_area.y + kWorkspaceBottomPanelCommandTopPadding;
    const std::string status_text = CommandPromptCoordinator::PromptStatusText(
        *panel_vm.command_state);
    DrawTextOn(text_renderer_, renderer, command_area.x + 12.0f, status_y, theme_.text_muted,
               theme_.surface_raised, TruncateLabel(status_text, command_area.w - 24.0f));

    const TextInputSurface panel_surface = TextInputSurface::Command;
    const TextInputSurface current_surface = text_input_vm.current_surface;
    const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
    DrawTextFieldFrame(renderer, theme_, prompt_rect, current_surface == panel_surface);
    const auto visual =
        (current_surface == panel_surface) ? BuildActiveTextInputVisual(layout, std::nullopt)
                                           : std::nullopt;
    // Avoid materializing "> "+input every frame: assemble into a thread_local scratch on the
    // fallback path only when the visual hasn't supplied displayed text.
    thread_local std::string panel_fallback_scratch;
    std::string_view panel_display_text;
    if (visual.has_value() && !visual->displayed_text.empty()) {
      panel_display_text = visual->displayed_text;
    } else {
      panel_fallback_scratch.clear();
      panel_fallback_scratch.reserve(2 + panel_vm.command_state->input.text().size());
      panel_fallback_scratch.append("> ");
      panel_fallback_scratch.append(panel_vm.command_state->input.text());
      panel_display_text = panel_fallback_scratch;
    }
    DrawSingleLineTextTail(
        renderer, prompt_rect.x + 6.0f,
        prompt_rect.y + std::floor((prompt_rect.h - text_renderer_.LineHeight()) * 0.5f),
                           std::max(1.0f, prompt_rect.w - 12.0f), theme_.text_primary,
                           theme_.surface_background, panel_display_text);
  }

  if (panel_layout.scroll.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, panel_layout.scroll.vertical_scrollbar->track,
                  panel_layout.scroll.vertical_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar);
  }
  if (panel_vm.project_state->surface.focus == FocusTarget::Panel) {
    DrawSurfaceFocusRing(renderer, layout.bottom_panel);
  }
}

}  // namespace microide::workspace
