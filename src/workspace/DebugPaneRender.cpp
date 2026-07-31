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
  // Skipped entirely when the debugger is disabled: PrepareFrameOnce leaves the
  // cached view model empty in that case.
  if (!prepare_cached_debug_pane_vm_.has_value()) {
    return;
  }
  const DebugPaneSurfaceViewModel& pane_vm = *prepare_cached_debug_pane_vm_;
  if (!pane_vm.visible || layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f) {
    return;
  }

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
    DrawModeTab(text_renderer_, renderer, theme_, tab.rect,
                last_mouse_position_valid_ && Contains(tab.rect, last_mouse_x_, last_mouse_y_),
                tab.mode == pane_vm.mode,
                mode_row.icon_only ? std::string_view{} : tab.label,
                [&](const SDL_FRect& icon_rect, SDL_Color color) {
                  draw_mode_glyph(tab.mode, icon_rect, color);
                });
  }

  // Narrow per-mode model pointers wired by the builder — no broad state access.
  const DebugExecutionView* debug_view = pane_vm.execution;
  const DebugVariablesModel* vars_model = pane_vm.variables;
  const DebugWatchModel* watch_model = pane_vm.watch;
  const DebugBreakpointsModel* breakpoints_model = pane_vm.breakpoints;
  const int bp_selected_row = pane_vm.breakpoints_selected_row;

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
               text_renderer_.TruncateToWidthEphemeralView(primary, text_width));
    if (secondary.empty()) {
      return;
    }
    const float secondary_x = x + primary_w + 12.0f;
    const float secondary_w = x + text_width - secondary_x;
    if (secondary_w > 0.0f) {
      DrawTextOn(text_renderer_, renderer, secondary_x, line_y, secondary_color, background,
                 text_renderer_.TruncateToWidthEphemeralView(secondary, secondary_w));
    }
  };
  // Disclosure triangle for an expandable Variables/Watch row.
  const auto draw_disclosure = [&](float x, float line_y, bool expanded, SDL_Color color) {
    const float size = 7.0f;
    const float cy = line_y + panel_layout.line_height * 0.5f - 1.0f;
    render::SetDrawColor(renderer, color);
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
  // Map a value's kind to its display color. Reuses the editor syntax palette so
  // numbers / strings / pointers read the same in the debugger as in source.
  const auto value_kind_color = [&](DebugValueKind kind) -> SDL_Color {
    switch (kind) {
      case DebugValueKind::Number:
        return theme_.syntax_number;
      case DebugValueKind::String:
        return theme_.syntax_string;
      case DebugValueKind::Pointer:
        return theme_.syntax_constant;
      case DebugValueKind::Boolean:
        return theme_.syntax_keyword;
      case DebugValueKind::Aggregate:
      case DebugValueKind::Plain:
      case DebugValueKind::Scope:
      case DebugValueKind::Pending:
      case DebugValueKind::Error:
        return theme_.text_secondary;
    }
    return theme_.text_secondary;
  };
  // Shared row painter for the lazy value-tree surfaces (Variables + Watch): an
  // indented disclosure for expandable rows, then name + value + a dim trailing
  // type hint, with an inline setVariable editor over the value column while a
  // leaf is being edited.
  const auto draw_value_tree_row = [&](const auto& model, std::size_t row_index, float line_y) {
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    if (row_index >= rows.size()) {
      return;  // Defensive: keep parity with the breakpoints branch's bounds guard.
    }
    const DebugVariableRowView& var_row = rows[row_index];
    const bool selected = row_index == model.SelectedRow();
    const SDL_FRect row_band = MakeRect(panel_layout.content_rect.x, line_y - 1.0f,
                                        panel_layout.content_rect.w, panel_layout.line_height);
    SDL_Color background = theme_.surface_background;
    if (selected || PointerOver(row_band)) {
      background = theme_.row_highlight;
      DrawFilledRect(renderer, row_band, background);
    }
    const float indent = static_cast<float>(var_row.depth) * kDebugPaneTreeIndentStep;
    const float row_x = panel_layout.text_x + indent;
    const float name_x = row_x + kDebugPaneTreeDisclosureSlot;
    const float content_right = panel_layout.text_x + panel_layout.text_width;
    const float name_avail = content_right - name_x;
    if (name_avail <= 0.0f) {
      return;
    }
    // Synthetic "loading…"/"<unavailable>" placeholder: dim, no disclosure, inert.
    if (var_row.is_placeholder) {
      DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.text_muted, background,
                 text_renderer_.TruncateToWidthEphemeralView(var_row.display_name, name_avail));
      return;
    }
    // Synthetic "show more…" affordance: accented to read as clickable; the click
    // routes through ToggleRow like any other row and fetches the next page.
    if (var_row.is_show_more) {
      DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.accent, background,
                 text_renderer_.TruncateToWidthEphemeralView(var_row.display_name, name_avail));
      return;
    }
    if (var_row.has_children) {
      draw_disclosure(row_x, line_y, var_row.expanded, theme_.text_muted);
    }
    const bool editing = model.IsEditing() && model.EditingNodeId().has_value() &&
                         *model.EditingNodeId() == var_row.node_id;
    if (editing) {
      const float name_w = text_renderer_.MeasureWidth(var_row.display_name);
      DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.text_primary, background,
                 text_renderer_.TruncateToWidthEphemeralView(var_row.display_name, name_avail));
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
    // Scope rows (Locals / Registers) read as section headers: accent name, no
    // value/type. Variable rows draw name | kind-colored value | dim type hint,
    // with the value column loosely aligned and the type hint dropped first when
    // space is tight, then the value, then the name.
    if (var_row.kind == DebugValueKind::Scope) {
      DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.accent, background,
                 text_renderer_.TruncateToWidthEphemeralView(var_row.display_name, name_avail));
      return;
    }
    const float name_w = text_renderer_.MeasureWidth(var_row.display_name);
    DrawTextOn(text_renderer_, renderer, name_x, line_y, theme_.text_primary, background,
               text_renderer_.TruncateToWidthEphemeralView(var_row.display_name, name_avail));
    // Reserve a dim, right-aligned type hint when one is present and fits.
    float values_right = content_right;
    if (!var_row.display_type.empty()) {
      const float type_w = text_renderer_.MeasureWidth(var_row.display_type);
      const float type_x = content_right - type_w;
      const float min_value_room = 48.0f;  // keep the value legible before showing a type
      if (type_x > name_x + name_w + 12.0f + min_value_room) {
        DrawTextOn(text_renderer_, renderer, type_x, line_y, theme_.text_muted, background,
                   var_row.display_type);
        values_right = type_x - 8.0f;
      }
    }
    if (var_row.display_value.empty()) {
      return;
    }
    // Loosely align the value column (~40% in); never let it overlap a long name.
    const float aligned_value_x =
        panel_layout.text_x + std::max(120.0f, panel_layout.text_width * 0.40f);
    const float value_x = std::max(aligned_value_x, name_x + name_w + 12.0f);
    const float value_w = values_right - value_x;
    if (value_w > 0.0f) {
      DrawTextOn(
          text_renderer_, renderer, value_x, line_y, value_kind_color(var_row.kind), background,
          text_renderer_.TruncateToWidthEphemeralView(var_row.display_value, value_w));
    }
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
        const SDL_FRect row_band = MakeRect(panel_layout.content_rect.x, line_y - 1.0f,
                                            panel_layout.content_rect.w, panel_layout.line_height);
        SDL_Color background = theme_.surface_background;
        if (focused || PointerOver(row_band)) {
          background = theme_.row_highlight;
          DrawFilledRect(renderer, row_band, background);
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
        DrawTextOn(
            text_renderer_, renderer, panel_layout.text_x, line_y, text, background,
            text_renderer_.TruncateToWidthEphemeralView(session.display, panel_layout.text_width));
        continue;
      }
      const float thread_indent = debug_view->HasSessionSelector() ? kIndentStep : 0.0f;
      if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
        const DebugThreadView& thread = debug_view->threads[row_ref.index];
        const bool focused = thread.id == debug_view->focused_thread_id;
        const SDL_Color background = fill_row_background(focused);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x + thread_indent, line_y,
                   focused ? theme_.text_primary : theme_.text_secondary, background,
                   text_renderer_.TruncateToWidthEphemeralView(thread.display,
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
        DrawTextOn(
            text_renderer_, renderer, panel_layout.text_x, line_y, theme_.text_muted,
            theme_.surface_background,
            text_renderer_.TruncateToWidthEphemeralView(bp_row.display, panel_layout.text_width));
        continue;
      }
      // Non-header breakpoint rows are clickable (toggle/edit), so lift the row band on
      // hover and use that as the text background for the whole row. The keyboard
      // selection lifts it the same way, matching the value trees one mode over —
      // without it the Breakpoints surface would navigate invisibly.
      const SDL_FRect bp_band = MakeRect(panel_layout.content_rect.x, line_y - 1.0f,
                                         panel_layout.content_rect.w, panel_layout.line_height);
      SDL_Color bp_bg = theme_.surface_background;
      if (bp_selected_row == index || PointerOver(bp_band)) {
        bp_bg = theme_.row_highlight;
        DrawFilledRect(renderer, bp_band, bp_bg);
      }
      if (bp_row.kind == DebugBreakpointRowView::Kind::ExceptionFilter) {
        const char* checkbox = bp_row.enabled ? "[x] " : "[ ] ";
        const float box_w = text_renderer_.MeasureWidth(checkbox);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x, line_y,
                   bp_row.enabled ? theme_.text_primary : theme_.text_secondary,
                   bp_bg, checkbox);
        DrawTextOn(text_renderer_, renderer, panel_layout.text_x + box_w, line_y,
                   bp_row.enabled ? theme_.text_primary : theme_.text_secondary,
                   bp_bg,
                   text_renderer_.TruncateToWidthEphemeralView(bp_row.display,
                                                  panel_layout.text_width - box_w));
        continue;
      }
      // Enabled checkbox prefix (double-click toggles it). A disabled breakpoint
      // dims; one the adapter rejected reads in the warning tint with the reason in
      // the muted trailer.
      const char* bp_checkbox = bp_row.enabled ? "[x] " : "[ ] ";
      const float bp_box_w = text_renderer_.MeasureWidth(bp_checkbox);
      const float bp_x = panel_layout.text_x + kDebugPaneBreakpointIndent;
      const SDL_Color bp_primary = bp_row.failed       ? theme_.diagnostic_warning
                                   : bp_row.enabled    ? theme_.text_secondary
                                                       : theme_.text_disabled;
      DrawTextOn(text_renderer_, renderer, bp_x, line_y, bp_primary, bp_bg, bp_checkbox);
      draw_two_column_row(bp_x + bp_box_w,
                          panel_layout.text_width - kDebugPaneBreakpointIndent - bp_box_w,
                          bp_row.display, bp_primary, bp_row.secondary, theme_.text_muted, line_y,
                          bp_bg);
      continue;
    }
  }

  // Empty-state hints. Static literals, so no per-frame string materialization,
  // and word-wrapped: the pane is a ~270px rail and these sentences are written
  // to say what would put a row here, which does not fit on one line. Every mode
  // has one — Variables used to have none, so an idle pane painted as a blank
  // box with no explanation at all.
  const auto draw_hint = [&](std::string_view text) {
    DrawWrappedPlaceholder(text_renderer_, renderer, panel_layout.text_x, panel_layout.text_y,
                           panel_layout.text_width, theme_.text_muted, theme_.surface_background,
                           text);
  };
  if (watch_model != nullptr && watch_model->Rows().empty()) {
    draw_hint("No watch expressions — click or press Insert to add one.");
  }
  if (breakpoints_model != nullptr && breakpoints_model->RowCount() == 0) {
    draw_hint("No breakpoints — click the editor gutter to add one.");
  }
  if (vars_model != nullptr && line_count == 0) {
    draw_hint("No variables — start a debug session and pause to inspect scope.");
  }
  if (debug_view != nullptr && line_count == 0) {
    draw_hint("Not paused — start a debug session and hit a breakpoint.");
  }

  // Same geometry the grab path hit-tests (panel_layout.scroll), not a second
  // hand-rolled MakeVerticalScrollbarGeometry call that could drift from it.
  if (panel_layout.scroll.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, panel_layout.scroll.vertical_scrollbar->track,
                  panel_layout.scroll.vertical_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::DebugPaneScrollbar);
  }

  // The debug pane is a keyboard focus target (arrows/Enter drive its rows), so it
  // marks focus the same way the other three focusable surfaces do. It was the one
  // surface that took focus without ever saying so.
  if (pane_vm.focus == FocusTarget::DebugPane) {
    DrawSurfaceFocusRing(renderer, layout.right_pane);
  }
}

}  // namespace microide::workspace
