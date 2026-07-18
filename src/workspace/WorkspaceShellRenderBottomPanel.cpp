#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <variant>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "render/PluginDisplayList.h"
#include "render/PluginDisplayListRenderer.h"
#include "render/ScopedRenderClip.h"
#include "render/SurfaceTextureCache.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

using namespace detail;

namespace {

const std::vector<terminal::TerminalLine>& EmptyTerminalLines() {
  static const std::vector<terminal::TerminalLine> empty_lines;
  return empty_lines;
}

// Paint a decoded raster surface (scaled to fit, clipped to `rect`), or a muted
// "Rendering…" placeholder while its decode is still in flight. Split out of
// RenderPluginSurfaceInto so the display-list and raster paths stay independently
// readable; kept a file-local helper so it adds no member to the size-capped shell.
void RenderRasterSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                             const editor::RasterHandle& raster, float scroll_y,
                             render::SurfaceTextureCache& cache, render::TextRenderer& text_renderer,
                             const render::Theme& theme) {
  constexpr float kPad = 8.0f;
  const render::SurfaceTextureCache::Entry* entry = cache.Lookup(raster.content_hash);
  if (entry == nullptr || entry->texture == nullptr) {
    text_renderer.DrawString(renderer, rect.x + kPad, rect.y + kPad, theme.text_muted, "Rendering…");
    return;
  }
  const float avail_w = std::max(1.0f, rect.w - kPad * 2.0f);
  const float scale =
      entry->width > 0 ? std::min(1.0f, avail_w / static_cast<float>(entry->width)) : 1.0f;
  const SDL_FRect dest{rect.x + kPad, rect.y + kPad - scroll_y,
                       static_cast<float>(entry->width) * scale,
                       static_cast<float>(entry->height) * scale};
  const SDL_Rect clip{static_cast<int>(std::floor(rect.x)), static_cast<int>(std::floor(rect.y)),
                      static_cast<int>(std::ceil(rect.w)), static_cast<int>(std::ceil(rect.h))};
  const render::ScopedRenderClip clip_scope(renderer, clip);
  SDL_RenderTexture(renderer, entry->texture, nullptr, &dest);
}

}  // namespace

void WorkspaceShell::RenderBottomPanelSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout,
                                              std::size_t terminal_line_count) {
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  if (panel_vm.content == PanelContentKind::None) {
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
      .hover_fill = theme_.row_highlight,
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

    const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
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
                              terminal_text_renderer_.LineHeight()),
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
      terminal_text_renderer_.DrawString(renderer, run_x, y, run.foreground, run_text);
    }

    // Underline / double-underline / strikethrough decorations. Run-length
    // coalesced over (foreground color, decoration bits) so a styled span emits
    // a single filled rect rather than one per cell. Wide trailing spacers carry
    // the lead cell's style, so the decoration spans both columns of a wide glyph.
    constexpr std::uint16_t kDecorationMask = terminal::cell_attr::kUnderline |
                                              terminal::cell_attr::kDoubleUnderline |
                                              terminal::cell_attr::kStrikethrough;
    const float line_height = terminal_text_renderer_.LineHeight();
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
    const auto& panel_slide = panel_vm.tab_drag;
    const auto panel_slide_dx = [&](std::size_t index) -> float {
      return panel_slide.sliding && index < panel_slide.offsets.size() ? panel_slide.offsets[index]
                                                                       : 0.0f;
    };
    for (const VisibleStripTab& tab : visible_panel_tabs) {
      if (panel_slide.active && tab.index == panel_slide.source_index) {
        continue;  // lifted; rendered as the floating ghost below
      }
      const float dx = panel_slide_dx(tab.index);
      SDL_FRect rect = tab.rect;
      SDL_FRect close_rect = tab.close_rect;
      rect.x += dx;
      close_rect.x += dx;
      DrawStripTab(text_renderer_, renderer, theme_, rect, tab.display_title, tab.badge_text,
                   tab.badge_color, tab.show_badge, tab.active,
                   StripTabStyle{
                       .text_left_padding = 8.0f,
                       .close_right_reserve = 40.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   panel_tab_palette, PointerOver(rect));
      draw_tab_close_button(close_rect,
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
                          panel_vm.tab_drag.source_index,
                          panel_vm.tab_drag.pointer_x, panel_vm.tab_drag.grab_offset_x,
                          StripTabStyle{
                              .text_left_padding = 8.0f,
                              .close_right_reserve = 40.0f,
                              .accent_edge = StripAccentEdge::Top,
                          },
                          panel_tab_palette);
    }
  } else {
    // Header label is one of: "Terminal" (default — a Terminal panel with no live tabs),
    // "Output" (no channel match), or the channel's label/id. Hold via string_view so the
    // constant case allocates nothing per frame.
    std::string_view header_label = "Terminal";
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

  // Phase E0: a plugin content surface replaces the terminal/output body. The
  // host owns scroll + clipping; the plugin only supplied data (display list or
  // raster handle). Reached through the view model's project_state pointer (the
  // sanctioned escape hatch), never the shell's live project state.
  if (panel_vm.content == PanelContentKind::PluginSurface && panel_vm.project_state != nullptr) {
    const SDL_FRect body =
        MakeRect(layout.bottom_panel.x, panel_header.y + panel_header.h, layout.bottom_panel.w,
                 std::max(0.0f, layout.bottom_panel.h - panel_header.h));
    FillRect(renderer, body, theme_.surface_background);
    const PanelState& panel = panel_vm.project_state->panel;
    const auto* pres = panel_vm.project_state->plugin_presentation_if_present();
    const editor::SurfaceContent* content =
        pres != nullptr ? pres->surfaces.Find(panel.surface_owner, panel.surface_id) : nullptr;
    RenderPluginSurfaceInto(renderer, body, content,
                            static_cast<float>(panel.surface_scroll_y));
    return;
  }

  const std::vector<std::string>* output_entries =
      output_panel ? OutputChannelEntries(panel_vm.output_channel_id)
                   : nullptr;
  std::optional<std::filesystem::path> current_reference_path;
  const std::size_t panel_line_count =
      terminal_panel ? terminal_line_count
                     : output_panel ? (output_entries != nullptr ? output_entries->size() : 0)
                     : 0;

  if (terminal_panel) {
    // Rebase the terminal's absolute-row mirrors for any scrollback trimmed since the
    // last frame BEFORE the layout reads scroll_row, so a scrolled-up view / selection
    // tracks the same content instead of jumping forward by the trimmed batch.
    RebaseActiveTerminalForScrollbackTrim();
  }
  const LogSurfaceLayout panel_layout =
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
          // Resolved+normalized path is memoized per entry in the output channel layer
          // (keyed by project root), so this is a cache hit per paint (TD-2026-07-17A-017).
          current_reference_path = output_channels_.ResolvedReferencePath(
              panel_vm.output_channel_id, output_index, panel_vm.project_root);
        } else if (parsed->kind == WorkspaceOutputChannels::ParsedEntry::Kind::ContextSnippet) {
          current_reference_path = output_channels_.ResolvedReferencePath(
              panel_vm.output_channel_id, output_index, panel_vm.project_root);

          const std::string_view visible_prefix =
              text_renderer_.TruncateToWidthEphemeralView(parsed->prefix, panel_layout.text_width);
          DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_secondary,
                     theme_.surface_background, visible_prefix);

          if (visible_prefix.size() == parsed->prefix.size()) {
            const float prefix_width = text_renderer_.MeasureWidth(parsed->prefix);
            const float remaining_width = panel_layout.text_width - prefix_width;
            if (remaining_width > 0.0f && !parsed->code.empty()) {
              const std::string_view visible_code =
                  text_renderer_.TruncateToWidthEphemeralView(parsed->code, remaining_width);
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
                 text_renderer_.TruncateToWidthEphemeralView(output_line, panel_layout.text_width));
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
        const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
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
                terminal_text_renderer_.DrawString(renderer, cursor_x, cursor_y, cursor_foreground,
                                                   display_text);
              }
            }
          }
        }
      }
    }
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

// Host-owned painter for a single plugin content surface, shared by the bottom
// preview panel (E0) and inline insets (E1). The surface carries only data (a
// display list or a raster handle); all drawing happens here. `scroll_y` is the
// host-owned vertical scroll within the surface. Reads no project state.
void WorkspaceShell::RenderPluginSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                                             const editor::SurfaceContent* content,
                                             float scroll_y) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  // Make any freshly decoded rasters available before we look them up.
  surface_texture_cache_.Upload(renderer);

  constexpr float kPad = 8.0f;
  if (content == nullptr || !content->has_body()) {
    return;
  }

  if (const auto* display_list = std::get_if<render::PluginDisplayList>(&content->body)) {
    render::DisplayListReplayParams params;
    params.origin_x = rect.x + kPad;
    params.origin_y = rect.y + kPad - scroll_y;
    params.clip = rect;
    render::ReplayDisplayList(renderer, text_renderer_, surface_texture_cache_, *display_list,
                              params);
    return;
  }

  if (const auto* raster = std::get_if<editor::RasterHandle>(&content->body)) {
    RenderRasterSurfaceInto(renderer, rect, *raster, scroll_y, surface_texture_cache_,
                            text_renderer_, theme_);
  }
}

// Phase E1 (gated): paint inline-surface insets into the inert row-gaps the view
// model carries. Uses the same EditorRowYLayout mapping the text-row loop used,
// so an inset always lands directly below its anchor row. Reads only the view
// model + metrics, never project state.
void WorkspaceShell::DrawEditorInsets(SDL_Renderer* renderer, const SDL_FRect& pane_rect,
                                      const editor::EditorViewMetrics& metrics,
                                      std::size_t scroll_line,
                                      const editor::EditorViewModel& view_model) {
  if (view_model.row_gaps.empty()) {
    return;
  }
  const editor::EditorRowYLayout layout(metrics.first_line_y, metrics.line_height,
                                        static_cast<std::uint32_t>(scroll_line),
                                        view_model.row_gaps);
  for (std::size_t i = 0; i < view_model.row_gaps.size(); ++i) {
    const editor::RowGap& gap = view_model.row_gaps[i];
    if (gap.visual_row < scroll_line) {
      continue;
    }
    const std::size_t row = gap.visual_row - scroll_line;
    const editor::RowGapContent content = i < view_model.row_gap_contents.size()
                                              ? view_model.row_gap_contents[i]
                                              : editor::RowGapContent{};
    if (gap.placement == editor::RowGapPlacement::Above) {
      // Phase E2: above-line code-lens strip sits directly over the row's text.
      const float top = layout.RowTop(row) - gap.height;
      if (content.code_lens != nullptr && !content.code_lens->text.empty()) {
        const float text_x = pane_rect.x + metrics.gutter_width + 4.0f;
        const float text_y = top + std::max(0.0f, (gap.height - metrics.line_height) * 0.5f);
        text_renderer_.DrawString(renderer, text_x, text_y, theme_.accent,
                                  content.code_lens->text);
      }
      continue;
    }
    const float top = layout.RowTop(row) + metrics.line_height;
    if (content.ghost_text != nullptr) {
      // Below-caret ghost-text rows, dimmed and aligned under the code (the first
      // line renders inline at the caret in EditorViewRenderer).
      for (std::size_t j = 0; j < content.ghost_text->below_lines.size(); ++j) {
        text_renderer_.DrawString(renderer, metrics.text_x,
                                  top + static_cast<float>(j) * metrics.line_height,
                                  theme_.text_muted, content.ghost_text->below_lines[j]);
      }
      continue;
    }
    const SDL_FRect inset{pane_rect.x, top, pane_rect.w, gap.height};
    RenderPluginSurfaceInto(renderer, inset, content.surface, 0.0f);
  }
}

}  // namespace microide::workspace
