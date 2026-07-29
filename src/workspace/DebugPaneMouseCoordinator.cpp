#include "workspace/DebugPaneMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "workspace/ListSelection.h"

namespace microide::workspace {

namespace {

// Resolve a click to an absolute row in the active surface, sharing the exact
// geometry the render and cursor paths use (DebugPaneRowAtPoint) so they cannot
// drift and the top text inset is not a dead-zone.
std::optional<std::size_t> DebugPaneLineIndexAtPoint(
    const WorkspaceShell::LogSurfaceLayout& panel_layout, float x, float y,
    std::size_t line_count) {
  const DebugPaneRowHit hit = DebugPaneRowAtPoint(
      panel_layout.content_rect, panel_layout.text_y, panel_layout.line_height,
      panel_layout.scroll.visible_rows, panel_layout.scroll.vertical_scroll, line_count, x, y);
  if (hit.row_index < 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(hit.row_index);
}

}  // namespace

DebugPaneMouseCoordinator::DebugPaneMouseCoordinator(ProjectWorkspaceState& state,
                                                     InteractionState& interaction_state,
                                                     Operations operations)
    : state_(state), interaction_state_(interaction_state), operations_(std::move(operations)) {}

bool DebugPaneMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                                 const WorkspaceLayout& layout) {
  if (!state_.debug_pane.visible || layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f) {
    return false;
  }
  const bool is_right = event.button.button == SDL_BUTTON_RIGHT;
  if (event.button.button != SDL_BUTTON_LEFT && !is_right) {
    return false;
  }
  if (!Contains(layout.right_pane, static_cast<float>(event.button.x),
                static_cast<float>(event.button.y))) {
    return false;
  }

  if (is_right) {
    return HandleRowContextMenu(event, layout);
  }

  // The scrollbar overlaps the row area, so it must claim the press before the
  // row hit test turns a bar grab into a frame/breakpoint activation.
  if (BeginScrollbarDrag(event, layout)) {
    return true;
  }

  // Mode-row button switcher: a click on a tab activates that surface.
  const DebugPaneModeRowLayout mode_row = operations_.debug_pane_mode_row(layout.right_pane);
  for (int i = 0; i < mode_row.tab_count; ++i) {
    const DebugPaneModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
    if (Contains(tab.rect, static_cast<float>(event.button.x),
                 static_cast<float>(event.button.y))) {
      if (operations_.show_debug_pane_mode) {
        operations_.show_debug_pane_mode(tab.mode);
      }
      return true;
    }
  }

  return HandleRowClick(event, layout);
}

// Focus the session, thread or frame a Call Stack row names, exactly as a click
// does. Shared with the keyboard so the two cannot answer the same row
// differently. `move_focus_to_editor` is the one difference between them: a click
// hands focus to the editor it just navigated, while an arrow key must leave focus
// in the pane or the next arrow key would go somewhere else.
void DebugPaneMouseCoordinator::ActivateCallStackRow(std::size_t line_index,
                                                     bool move_focus_to_editor) {
  DebugExecutionView& exec = state_.debug_execution;
  if (line_index >= exec.PanelRowCount()) {
    return;
  }
  const auto row_ref = exec.PanelRowAt(line_index);
  if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Session) {
    const int session_id = exec.sessions[row_ref.index].id;
    if (session_id != exec.focused_session_id && operations_.on_debug_session_focus_changed) {
      operations_.on_debug_session_focus_changed(session_id);
    }
    return;
  }
  if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
    const int thread_id = exec.threads[row_ref.index].id;
    if (thread_id != exec.focused_thread_id && operations_.on_debug_thread_focus_changed) {
      operations_.on_debug_thread_focus_changed(thread_id);
    }
    return;
  }
  exec.focused_frame_index = row_ref.index;
  const DebugStackFrameView& frame = exec.frames[row_ref.index];
  if (operations_.on_debug_frame_focus_changed) {
    operations_.on_debug_frame_focus_changed(frame.id);
  }
  if (frame.source_path.empty()) {
    return;
  }
  operations_.open_file(frame.source_path);
  if (editor::TextViewport* viewport = operations_.active_editor_viewport();
      viewport != nullptr) {
    viewport->MoveCursorTo(frame.line, 0);
  }
  // Opening the frame's file focuses the editor on its own, so the keyboard path
  // has to claim focus back rather than merely decline to hand it over.
  state_.surface.focus = move_focus_to_editor ? FocusTarget::Editor : FocusTarget::DebugPane;
}

bool DebugPaneMouseCoordinator::HandleRowClick(const SDL_Event& event,
                                               const WorkspaceLayout& layout) {
  const DebugPaneMode mode = state_.debug_pane.mode;

  if (mode == DebugPaneMode::CallStack) {
    DebugExecutionView& exec = state_.debug_execution;
    const std::size_t row_count = exec.PanelRowCount();
    if (row_count == 0) {
      return false;
    }
    const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, row_count);
    if (!Contains(panel_layout.content_rect, static_cast<float>(event.button.x),
                  static_cast<float>(event.button.y))) {
      return false;
    }
    const auto line_index =
        DebugPaneLineIndexAtPoint(panel_layout, static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y), row_count);
    if (!line_index.has_value() || *line_index >= row_count) {
      state_.surface.focus = FocusTarget::DebugPane;
      return true;
    }
    ActivateCallStackRow(*line_index, /*move_focus_to_editor=*/true);
    return true;
  }

  if (mode == DebugPaneMode::Variables) {
    DebugVariablesModel& model = state_.debug_variables;
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    if (rows.empty()) {
      return false;
    }
    const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, rows.size());
    if (!Contains(panel_layout.content_rect, static_cast<float>(event.button.x),
                  static_cast<float>(event.button.y))) {
      return false;
    }
    const auto line_index =
        DebugPaneLineIndexAtPoint(panel_layout, static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y), rows.size());
    if (line_index.has_value() && *line_index < rows.size()) {
      model.SetSelectedRow(*line_index);
      const DebugVariableRowView& row = rows[*line_index];
      if (event.button.clicks >= 2 && row.editable && !row.has_children) {
        if (operations_.begin_debug_variable_edit) {
          operations_.begin_debug_variable_edit(*line_index);
        }
      } else if (event.button.clicks == 1 && (row.has_children || row.is_show_more)) {
        // A "show more…" row has no children but toggling it loads the next page
        // (ToggleRow handles it); without this the row was drawn/cursored as
        // clickable but did nothing, stranding children past the page size.
        if (operations_.toggle_debug_variable_row) {
          operations_.toggle_debug_variable_row(*line_index);
        }
      }
    }
    state_.surface.focus = FocusTarget::DebugPane;
    return true;
  }

  if (mode == DebugPaneMode::Watch) {
    DebugWatchModel& model = state_.debug_watch;
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    const auto panel_layout =
        operations_.compute_debug_pane_list_layout(layout, std::max<std::size_t>(rows.size(), 1));
    if (!Contains(panel_layout.content_rect, static_cast<float>(event.button.x),
                  static_cast<float>(event.button.y))) {
      return false;
    }
    const auto line_index =
        DebugPaneLineIndexAtPoint(panel_layout, static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y), rows.size());
    if (line_index.has_value() && *line_index < rows.size()) {
      model.SetSelectedRow(*line_index);
      const DebugVariableRowView& row = rows[*line_index];
      const std::optional<std::size_t> expr_index = model.ExpressionIndexForRow(*line_index);
      if (event.button.clicks >= 2 && expr_index.has_value()) {
        if (operations_.edit_debug_watch_expression) {
          operations_.edit_debug_watch_expression(*expr_index);
        }
      } else if (event.button.clicks >= 2 && row.editable && !row.has_children) {
        if (operations_.begin_debug_watch_edit) {
          operations_.begin_debug_watch_edit(*line_index);
        }
      } else if (event.button.clicks == 1 && (row.has_children || row.is_show_more)) {
        // "show more…" rows page in the next batch via ToggleRow (see Variables above).
        if (operations_.toggle_debug_watch_row) {
          operations_.toggle_debug_watch_row(*line_index);
        }
      }
    } else if (operations_.add_debug_watch_expression) {
      operations_.add_debug_watch_expression();
    }
    state_.surface.focus = FocusTarget::DebugPane;
    return true;
  }

  if (mode == DebugPaneMode::Breakpoints) {
    const DebugBreakpointsModel& model = state_.debug_breakpoints_panel;
    const std::vector<DebugBreakpointRowView>& rows = model.Rows();
    if (rows.empty()) {
      return false;
    }
    const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, rows.size());
    if (!Contains(panel_layout.content_rect, static_cast<float>(event.button.x),
                  static_cast<float>(event.button.y))) {
      return false;
    }
    const auto line_index =
        DebugPaneLineIndexAtPoint(panel_layout, static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y), rows.size());
    if (line_index.has_value() && *line_index < rows.size()) {
      const DebugBreakpointRowView& row = rows[*line_index];
      if (row.kind == DebugBreakpointRowView::Kind::ExceptionFilter &&
          operations_.toggle_debug_exception_filter) {
        operations_.toggle_debug_exception_filter(row.filter_id);
      } else if (row.kind == DebugBreakpointRowView::Kind::FunctionBreakpoint) {
        // No source location to navigate to: a click toggles enabled.
        if (operations_.toggle_debug_function_breakpoint_enabled) {
          operations_.toggle_debug_function_breakpoint_enabled(row.function_index);
        }
        state_.surface.focus = FocusTarget::DebugPane;
        return true;
      } else if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && !row.path.empty()) {
        // Double-click toggles enabled (manage from the panel); single-click
        // navigates to the breakpoint's source line.
        if (event.button.clicks >= 2) {
          if (operations_.toggle_debug_breakpoint_enabled) {
            operations_.toggle_debug_breakpoint_enabled(row.path, row.line);
          }
          state_.surface.focus = FocusTarget::DebugPane;
          return true;
        }
        operations_.open_file(row.path);
        if (editor::TextViewport* viewport = operations_.active_editor_viewport();
            viewport != nullptr) {
          viewport->MoveCursorTo(row.line, 0);
        }
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
    }
    state_.surface.focus = FocusTarget::DebugPane;
    return true;
  }

  return false;
}

bool DebugPaneMouseCoordinator::HandleRowContextMenu(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  const DebugPaneMode mode = state_.debug_pane.mode;
  if (mode == DebugPaneMode::CallStack) {
    return true;  // Frames/threads/sessions have no row-scoped commands yet.
  }

  const std::size_t row_count = operations_.debug_pane_active_row_count();
  if (row_count == 0) {
    return true;
  }
  const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, row_count);
  const auto line_index =
      DebugPaneLineIndexAtPoint(panel_layout, static_cast<float>(event.button.x),
                                static_cast<float>(event.button.y), row_count);
  if (!line_index.has_value() || *line_index >= row_count) {
    return true;
  }

  const SDL_FRect anchor = MakeRect(static_cast<float>(event.button.x),
                                    static_cast<float>(event.button.y), 1.0f, 1.0f);
  // Select the row first, then open on the selection — the order every other list
  // surface uses, so the menu and the highlight cannot disagree.
  state_.surface.focus = FocusTarget::DebugPane;

  if (mode == DebugPaneMode::Breakpoints) {
    const std::vector<DebugBreakpointRowView>& rows = state_.debug_breakpoints_panel.Rows();
    if (*line_index >= rows.size()) {
      return true;
    }
    const DebugBreakpointRowView& row = rows[*line_index];
    // Only line breakpoints carry the (path, line) the gutter menu acts on;
    // function breakpoints and exception filters have no source location.
    if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && !row.path.empty() &&
        operations_.open_breakpoint_context_menu) {
      operations_.open_breakpoint_context_menu(row.path, row.line, anchor);
    }
    return true;
  }

  if (mode == DebugPaneMode::Variables) {
    state_.debug_variables.SetSelectedRow(*line_index);
  } else {
    state_.debug_watch.SetSelectedRow(*line_index);
  }
  if (operations_.open_debug_value_context_menu) {
    operations_.open_debug_value_context_menu(anchor);
  }
  return true;
}

bool DebugPaneMouseCoordinator::BeginScrollbarDrag(const SDL_Event& event,
                                                   const WorkspaceLayout& layout) {
  const std::size_t line_count = operations_.debug_pane_active_row_count();
  const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, line_count);
  if (!panel_layout.scroll.vertical_scrollbar.has_value() ||
      !Contains(VerticalScrollbarHitRect(*panel_layout.scroll.vertical_scrollbar),
                static_cast<float>(event.button.x), static_cast<float>(event.button.y))) {
    return false;
  }

  interaction_state_.drag_target = DragTarget::DebugPaneScrollbar;
  interaction_state_.drag_scrollbar_offset =
      Contains(panel_layout.scroll.vertical_scrollbar->thumb, static_cast<float>(event.button.x),
               static_cast<float>(event.button.y))
          ? static_cast<float>(event.button.y) - panel_layout.scroll.vertical_scrollbar->thumb.y
          : panel_layout.scroll.vertical_scrollbar->thumb.h * 0.5f;
  // Clicking the track jumps the thumb to the pointer, matching every other
  // scrollbar in the shell.
  operations_.set_debug_pane_scroll_row(
      std::clamp(static_cast<int>(std::lround(ScrollUnitsForPointer(
                     *panel_layout.scroll.vertical_scrollbar, static_cast<float>(event.button.y),
                     interaction_state_.drag_scrollbar_offset))),
                 0, panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  state_.surface.focus = FocusTarget::DebugPane;
  return true;
}

bool DebugPaneMouseCoordinator::HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout) {
  if (interaction_state_.drag_target != DragTarget::DebugPaneScrollbar ||
      !state_.debug_pane.visible) {
    return false;
  }

  const std::size_t line_count = operations_.debug_pane_active_row_count();
  const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, line_count);
  if (!panel_layout.scroll.vertical_scrollbar.has_value()) {
    interaction_state_.drag_target = DragTarget::None;
    return false;
  }

  operations_.set_debug_pane_scroll_row(
      std::clamp(static_cast<int>(std::lround(ScrollUnitsForPointer(
                     *panel_layout.scroll.vertical_scrollbar, static_cast<float>(event.motion.y),
                     interaction_state_.drag_scrollbar_offset))),
                 0, panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  state_.surface.focus = FocusTarget::DebugPane;
  return true;
}

bool DebugPaneMouseCoordinator::HandleWheel(const SDL_Event& event, const WorkspaceLayout& layout,
                                            int vertical_ticks) {
  if (!state_.debug_pane.visible || layout.right_pane.w <= 0.0f ||
      !Contains(layout.right_pane, static_cast<float>(event.wheel.mouse_x),
                static_cast<float>(event.wheel.mouse_y))) {
    return false;
  }
  const std::size_t line_count = operations_.debug_pane_active_row_count();
  const auto panel_layout = operations_.compute_debug_pane_list_layout(layout, line_count);
  operations_.set_debug_pane_scroll_row(
      std::clamp(panel_layout.scroll.vertical_scroll - vertical_ticks * kWheelScrollRows, 0,
                 panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  // Scrolling is not focusing — see SidebarMouseCoordinator::HandleWheel.
  return true;
}

}  // namespace microide::workspace
