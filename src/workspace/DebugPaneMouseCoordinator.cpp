#include "workspace/DebugPaneMouseCoordinator.h"

#include <algorithm>
#include <utility>
#include <vector>

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
                                                     Operations operations)
    : state_(state), operations_(std::move(operations)) {}

bool DebugPaneMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                                 const WorkspaceLayout& layout) {
  if (!state_.debug_pane.visible || layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f) {
    return false;
  }
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (!Contains(layout.right_pane, static_cast<float>(event.button.x),
                static_cast<float>(event.button.y))) {
    return false;
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
    const auto row_ref = exec.PanelRowAt(*line_index);
    if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Session) {
      const int session_id = exec.sessions[row_ref.index].id;
      if (session_id != exec.focused_session_id && operations_.on_debug_session_focus_changed) {
        operations_.on_debug_session_focus_changed(session_id);
      }
      return true;
    }
    if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
      const int thread_id = exec.threads[row_ref.index].id;
      if (thread_id != exec.focused_thread_id && operations_.on_debug_thread_focus_changed) {
        operations_.on_debug_thread_focus_changed(thread_id);
      }
      return true;
    }
    exec.focused_frame_index = row_ref.index;
    const DebugStackFrameView& frame = exec.frames[row_ref.index];
    if (operations_.on_debug_frame_focus_changed) {
      operations_.on_debug_frame_focus_changed(frame.id);
    }
    if (!frame.source_path.empty()) {
      operations_.open_file(frame.source_path);
      if (editor::TextViewport* viewport = operations_.active_editor_viewport();
          viewport != nullptr) {
        viewport->MoveCursorTo(frame.line, 0);
      }
      state_.surface.focus = FocusTarget::Editor;
    }
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
      } else if (event.button.clicks == 1 && row.has_children) {
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
      } else if (event.button.clicks == 1 && row.has_children) {
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
      std::clamp(panel_layout.scroll.vertical_scroll - vertical_ticks, 0,
                 panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  state_.surface.focus = FocusTarget::DebugPane;
  return true;
}

}  // namespace microide::workspace
