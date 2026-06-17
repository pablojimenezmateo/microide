#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/DebugPaneRegistry.h"

// Right-side debug pane render TU. The four structured debug surfaces (Call Stack,
// Variables, Watch, Breakpoints) moved here from the bottom panel. This file is a
// lint-covered render TU: it consumes only the DebugPaneSurfaceViewModel + member
// geometry helpers (defined in DebugPaneLayout.cpp) and must not read live project
// state directly (via the context) nor materialize per-frame strings.

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderDebugPaneSurface(SDL_Renderer* renderer,
                                            const WorkspaceLayout& layout) {
  const DebugPaneSurfaceViewModel& pane_vm = *prepare_cached_debug_pane_vm_;
  if (!pane_vm.visible || layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f ||
      pane_vm.project_state == nullptr) {
    return;
  }
  ProjectWorkspaceState& project_state = *pane_vm.project_state;

  util::PerformanceTrace::Scope pane_scope("WorkspaceShell::RenderDebugPane");

  // Pane background + a 1px divider on the left edge (where it meets the editor).
  DrawFilledRect(renderer, layout.right_pane, theme_.surface_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.right_pane.x - kWorkspaceDividerThickness, layout.right_pane.y,
                          kWorkspaceDividerThickness, layout.right_pane.h),
                 theme_.border);

  // Mode-row button switcher (Call Stack / Variables / Watch / Breakpoints).
  const auto draw_mode_glyph = [&](DebugPaneMode mode, const SDL_FRect& icon_rect,
                                   SDL_Color color) {
    const float cx = icon_rect.x + icon_rect.w * 0.5f;
    const float cy = icon_rect.y + icon_rect.h * 0.5f;
    switch (mode) {
      case DebugPaneMode::CallStack: {
        // Three stacked horizontal bars (a "list").
        const float bar_w = std::min(10.0f, icon_rect.w - 4.0f);
        const float bar_x = cx - bar_w * 0.5f;
        for (int i = -1; i <= 1; ++i) {
          DrawFilledRect(renderer, MakeRect(bar_x, cy + static_cast<float>(i) * 3.0f - 0.5f,
                                            bar_w, 1.5f),
                         color);
        }
        break;
      }
      case DebugPaneMode::Variables: {
        // An "equals" sign (two short bars).
        const float bar_w = std::min(9.0f, icon_rect.w - 4.0f);
        const float bar_x = cx - bar_w * 0.5f;
        DrawFilledRect(renderer, MakeRect(bar_x, cy - 2.5f, bar_w, 1.5f), color);
        DrawFilledRect(renderer, MakeRect(bar_x, cy + 1.0f, bar_w, 1.5f), color);
        break;
      }
      case DebugPaneMode::Watch:
        DrawSearchGlyph(renderer, icon_rect, color);
        break;
      case DebugPaneMode::Breakpoints:
        DrawGlyphDot(renderer, cx, cy, color);
        break;
    }
  };

  const DebugPaneModeRowLayout mode_row = DebugPaneModeRow(layout.right_pane);
  for (int i = 0; i < mode_row.tab_count; ++i) {
    const DebugPaneModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
    const bool active = tab.mode == pane_vm.mode;
    const bool hovered =
        last_mouse_position_valid_ && Contains(tab.rect, last_mouse_x_, last_mouse_y_);
    const ButtonColors colors = ResolveButtonColors(
        theme_, ButtonTone::Neutral,
        ButtonVisualState{.enabled = true, .hovered = hovered, .active = active});
    DrawFilledRect(renderer, tab.rect, colors.fill);
    DrawRect(renderer, tab.rect, colors.border);
    if (mode_row.icon_only) {
      draw_mode_glyph(tab.mode, tab.rect, colors.text);
    } else {
      const SDL_FRect icon_rect = MakeRect(tab.rect.x + 4.0f, tab.rect.y, 16.0f, tab.rect.h);
      draw_mode_glyph(tab.mode, icon_rect, colors.text);
      const float label_x = icon_rect.x + icon_rect.w + 1.0f;
      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(label_x, tab.rect.y, std::max(0.0f, tab.rect.x + tab.rect.w - label_x - 4.0f),
                   tab.rect.h),
          0.0f, colors.text, colors.fill, tab.label);
    }
  }

  const DebugPaneMode mode = pane_vm.mode;
  const DebugExecutionView* debug_view =
      mode == DebugPaneMode::CallStack ? &project_state.debug_execution : nullptr;
  const DebugVariablesModel* vars_model =
      mode == DebugPaneMode::Variables ? &project_state.debug_variables : nullptr;
  const DebugWatchModel* watch_model =
      mode == DebugPaneMode::Watch ? &project_state.debug_watch : nullptr;
  const DebugBreakpointsModel* breakpoints_model =
      mode == DebugPaneMode::Breakpoints ? &project_state.debug_breakpoints_panel : nullptr;

  const std::size_t line_count = DebugPaneActiveRowCount();
  const LogSurfaceLayout panel_layout = ComputeDebugPaneListLayout(layout, line_count);
  SetDebugPaneScrollRow(panel_layout.scroll.vertical_scroll, line_count,
                        panel_layout.scroll.visible_rows);

  // Shared two-column row painter: a primary label, then a muted secondary
  // trailing it (Call Stack frame name + location; Variables name + value).
  const auto draw_two_column_row = [&](float x, float text_width, const std::string& primary,
                                       SDL_Color primary_color, const std::string& secondary,
                                       SDL_Color secondary_color, float line_y,
                                       SDL_Color background) {
    const float primary_w = text_renderer_.MeasureWidth(primary);
    DrawTextOn(text_renderer_, renderer, x, line_y, primary_color, background,
               text_renderer_.TruncateToWidth(primary, text_width));
    if (secondary.empty()) {
      return;
    }
    const float secondary_x = x + primary_w + 12.0f;
    const float secondary_w = x + text_width - secondary_x;
    if (secondary_w > 0.0f) {
      DrawTextOn(text_renderer_, renderer, secondary_x, line_y, secondary_color, background,
                 text_renderer_.TruncateToWidth(secondary, secondary_w));
    }
  };
  // Disclosure triangle for an expandable Variables/Watch row.
  const auto draw_disclosure = [&](float x, float line_y, bool expanded, SDL_Color color) {
    const float size = 7.0f;
    const float cy = line_y + panel_layout.line_height * 0.5f - 1.0f;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    if (expanded) {
      for (int i = 0; i < static_cast<int>(size); ++i) {
        const float w = size - static_cast<float>(i) * 2.0f;
        if (w <= 0.0f) {
          break;
        }
        DrawFilledRect(renderer,
                       MakeRect(x + static_cast<float>(i), cy - 2.0f + static_cast<float>(i), w,
                                1.0f),
                       color);
      }
    } else {
      for (int i = 0; i < static_cast<int>(size); ++i) {
        const float h = size - static_cast<float>(i) * 2.0f;
        if (h <= 0.0f) {
          break;
        }
        DrawFilledRect(renderer, MakeRect(x + static_cast<float>(i), cy - h * 0.5f, 1.0f, h),
                       color);
      }
    }
  };
  // Shared row painter for the lazy value-tree surfaces (Variables + Watch): an
  // indented disclosure for expandable rows, then name + value, with an inline
  // setVariable editor over the value column while a leaf is being edited.
  const auto draw_value_tree_row = [&](const auto& model, std::size_t row_index, float line_y) {
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    const DebugVariableRowView& var_row = rows[row_index];
    const bool selected = row_index == model.SelectedRow();
    SDL_Color background = theme_.surface_background;
    if (selected) {
      background = theme_.row_highlight;
      DrawFilledRect(renderer,
                     MakeRect(panel_layout.content_rect.x, line_y - 1.0f,
                              panel_layout.content_rect.w, panel_layout.line_height),
                     background);
    }
    const float indent = static_cast<float>(var_row.depth) * 14.0f;
    const float row_x = panel_layout.text_x + indent;
    if (var_row.has_children) {
      draw_disclosure(row_x, line_y, var_row.expanded, theme_.text_muted);
    }
    const float name_x = row_x + 14.0f;
    const float name_avail = panel_layout.text_x + panel_layout.text_width - name_x;
    if (name_avail <= 0.0f) {
      return;
    }
    const bool editing = model.IsEditing() && model.EditingNodeId().has_value() &&
                         *model.EditingNodeId() == var_row.node_id;
    if (editing) {
      const float name_w = text_renderer_.MeasureWidth(var_row.display_name);
      DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.text_primary, background,
                 text_renderer_.TruncateToWidth(var_row.display_name, name_avail));
      const float value_x = name_x + std::min(name_w, name_avail) + 12.0f;
      const float value_w = panel_layout.text_x + panel_layout.text_width - value_x;
      if (value_w > 4.0f) {
        const SDL_FRect field =
            MakeRect(value_x - 2.0f, line_y - 2.0f, value_w, panel_layout.line_height + 2.0f);
        DrawTextFieldFrame(renderer, theme_, field, true);
        const float field_text_x = value_x + 2.0f;
        const float field_avail = std::max(1.0f, value_w - 8.0f);
        const auto metrics = ComputeSingleLineViewMetrics(model.EditBuffer(), "", field_avail);
        const std::string_view displayed = metrics.displayed_text;
        if (metrics.selection_bytes.has_value()) {
          const float sel_x =
              field_text_x +
              text_renderer_.MeasureWidth(displayed.substr(0, metrics.selection_bytes->first));
          const float sel_w = text_renderer_.MeasureWidth(
              displayed.substr(metrics.selection_bytes->first,
                               metrics.selection_bytes->second - metrics.selection_bytes->first));
          if (sel_w > 0.0f) {
            DrawFilledRect(renderer,
                           MakeRect(sel_x, line_y - 1.0f, sel_w, panel_layout.line_height),
                           theme_.selection_fill);
          }
        }
        text_renderer_.DrawString(renderer, field_text_x, line_y, theme_.text_primary, displayed);
        if (context_.interaction_state.window_has_input_focus &&
            context_.text_input.composition.text.empty()) {
          DrawFilledRect(renderer,
                         MakeRect(field_text_x + metrics.cursor_x, line_y - 1.0f, 1.5f,
                                  text_renderer_.LineHeight()),
                         theme_.cursor);
        }
      }
      return;
    }
    draw_two_column_row(name_x, name_avail, var_row.display_name, theme_.text_primary,
                        var_row.display_value, theme_.text_secondary, line_y, background);
  };

  for (int row = 0; row < panel_layout.scroll.visible_rows; ++row) {
    const int index = panel_layout.scroll.vertical_scroll + row;
    if (index >= static_cast<int>(line_count)) {
      break;
    }
    const float line_y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height;
    if (debug_view != nullptr) {
      constexpr float kIndentStep = 16.0f;
      const auto row_ref = debug_view->PanelRowAt(static_cast<std::size_t>(index));
      const auto fill_row_background = [&](bool focused) -> SDL_Color {
        SDL_Color background = theme_.surface_background;
        if (focused) {
          background = theme_.row_highlight;
          DrawFilledRect(renderer,
                         MakeRect(panel_layout.content_rect.x, line_y - 1.0f,
                                  panel_layout.content_rect.w, panel_layout.line_height),
                         background);
        }
        return background;
      };
      if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Session) {
        const DebugSessionView& session = debug_view->sessions[row_ref.index];
        const bool focused = session.id == debug_view->focused_session_id;
        const SDL_Color background = fill_row_background(focused);
        SDL_Color text = focused ? theme_.text_primary : theme_.text_secondary;
        if (session.attention && !focused) {
          text = theme_.accent;
        }
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, text, background,
                   text_renderer_.TruncateToWidth(session.display, panel_layout.text_width));
        continue;
      }
      const float thread_indent = debug_view->HasSessionSelector() ? kIndentStep : 0.0f;
      if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
        const DebugThreadView& thread = debug_view->threads[row_ref.index];
        const bool focused = thread.id == debug_view->focused_thread_id;
        const SDL_Color background = fill_row_background(focused);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x + thread_indent, line_y,
                   focused ? theme_.text_primary : theme_.text_secondary, background,
                   text_renderer_.TruncateToWidth(thread.display,
                                                  panel_layout.text_width - thread_indent));
        continue;
      }
      const std::size_t frame_index = row_ref.index;
      const DebugStackFrameView& frame = debug_view->frames[frame_index];
      const bool focused = frame_index == debug_view->focused_frame_index;
      const SDL_Color background = fill_row_background(focused);
      const float frame_indent =
          thread_indent + (debug_view->HasThreadSelector() ? kIndentStep : 0.0f);
      const float frame_x = panel_layout.text_x + frame_indent;
      const float frame_width = panel_layout.text_width - frame_indent;
      draw_two_column_row(frame_x, frame_width, frame.display_primary,
                          focused ? theme_.text_primary : theme_.text_secondary,
                          frame.display_secondary, theme_.text_muted, line_y, background);
      continue;
    }
    if (vars_model != nullptr) {
      draw_value_tree_row(*vars_model, static_cast<std::size_t>(index), line_y);
      continue;
    }
    if (watch_model != nullptr) {
      draw_value_tree_row(*watch_model, static_cast<std::size_t>(index), line_y);
      continue;
    }
    if (breakpoints_model != nullptr) {
      const std::size_t row_index = static_cast<std::size_t>(index);
      if (row_index >= breakpoints_model->Rows().size()) {
        continue;
      }
      const DebugBreakpointRowView& bp_row = breakpoints_model->Rows()[row_index];
      if (bp_row.kind == DebugBreakpointRowView::Kind::Header) {
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_muted,
                   theme_.surface_background,
                   text_renderer_.TruncateToWidth(bp_row.display, panel_layout.text_width));
        continue;
      }
      if (bp_row.kind == DebugBreakpointRowView::Kind::ExceptionFilter) {
        const char* checkbox = bp_row.enabled ? "[x] " : "[ ] ";
        const float box_w = text_renderer_.MeasureWidth(checkbox);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y,
                   bp_row.enabled ? theme_.text_primary : theme_.text_secondary,
                   theme_.surface_background, checkbox);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x + box_w, line_y,
                   bp_row.enabled ? theme_.text_primary : theme_.text_secondary,
                   theme_.surface_background,
                   text_renderer_.TruncateToWidth(bp_row.display,
                                                  panel_layout.text_width - box_w));
        continue;
      }
      draw_two_column_row(panel_layout.text_x + 16.0f, panel_layout.text_width - 16.0f,
                          bp_row.display, theme_.text_secondary, bp_row.secondary,
                          theme_.text_muted, line_y, theme_.surface_background);
      continue;
    }
  }

  // Empty-state hints (static literals, so no per-frame string materialization).
  if (watch_model != nullptr && watch_model->Rows().empty()) {
    DrawTextOn(text_renderer_, renderer, panel_layout.text_x, panel_layout.text_y,
               theme_.text_muted, theme_.surface_background,
               "No watch expressions — click or press Insert to add one.");
  }
  if (breakpoints_model != nullptr && breakpoints_model->RowCount() == 0) {
    DrawTextOn(text_renderer_, renderer, panel_layout.text_x, panel_layout.text_y,
               theme_.text_muted, theme_.surface_background,
               "No breakpoints — click the editor gutter to add one.");
  }
  if (debug_view != nullptr && line_count == 0) {
    DrawTextOn(text_renderer_, renderer, panel_layout.text_x, panel_layout.text_y,
               theme_.text_muted, theme_.surface_background,
               "Not paused — start a debug session and hit a breakpoint.");
  }

  if (const auto geometry = MakeVerticalScrollbarGeometry(
          panel_layout.content_rect, static_cast<float>(line_count),
          static_cast<float>(panel_layout.scroll.visible_rows),
          static_cast<float>(panel_layout.scroll.vertical_scroll));
      geometry.has_value()) {
    DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, false);
  }
}

}  // namespace microide::workspace
