#include "workspace/WorkspacePanelMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "workspace/TerminalPanelService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"

namespace microide::workspace {

namespace {

struct OutputLocation {
  std::filesystem::path path;
  std::size_t line = 0;
  std::size_t column = 0;
};

std::optional<OutputLocation> ParseOutputLocationLine(std::string_view text) {
  const std::size_t column_delimiter = text.rfind(':');
  if (column_delimiter == std::string_view::npos || column_delimiter == 0) {
    return std::nullopt;
  }
  const std::size_t line_delimiter = text.rfind(':', column_delimiter - 1);
  if (line_delimiter == std::string_view::npos || line_delimiter == 0) {
    return std::nullopt;
  }

  const std::string_view path_text = text.substr(0, line_delimiter);
  const std::string_view line_text =
      text.substr(line_delimiter + 1, column_delimiter - line_delimiter - 1);
  const std::string_view column_text = text.substr(column_delimiter + 1);

  const auto line = util::ParseSize(line_text);
  const auto column = util::ParseSize(column_text);
  if (path_text.empty() || !line.has_value() || !column.has_value() || *line == 0) {
    return std::nullopt;
  }

  return OutputLocation{
      .path = std::filesystem::path(path_text),
      .line = *line,
      .column = *column,
  };
}

std::optional<std::size_t> BottomPanelLineIndexAtPoint(
    const WorkspaceShell::BottomPanelLogLayout& panel_layout,
    float y,
    std::size_t line_count) {
  return BottomPanelLineIndexAtY(panel_layout.text_y, panel_layout.line_height,
                                 panel_layout.scroll.visible_rows,
                                 panel_layout.scroll.vertical_scroll, y, line_count);
}

}  // namespace

PanelMouseCoordinator::PanelMouseCoordinator(ProjectWorkspaceState& state,
                                             MenuSurfaceState& menu_state,
                                             InteractionState& interaction_state,
                                             Operations operations)
    : state_(state),
      menu_state_(menu_state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

bool PanelMouseCoordinator::HandleResizeButtonDown(const SDL_Event& event,
                                                   const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !operations_.bottom_panel_visible() ||
      !Contains(operations_.bottom_panel_resize_handle_rect(layout), event.button.x,
                event.button.y)) {
    return false;
  }
  interaction_state_.drag_target = DragTarget::BottomPanelDivider;
  return true;
}

bool PanelMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                             const WorkspaceLayout& layout) {
  if (event.button.button == SDL_BUTTON_LEFT && operations_.bottom_panel_visible()) {
    const std::size_t line_count = operations_.bottom_panel_line_count();
    const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
    if (panel_layout.scroll.vertical_scrollbar.has_value() &&
        Contains(panel_layout.scroll.vertical_scrollbar->track, event.button.x, event.button.y)) {
      interaction_state_.drag_target = DragTarget::BottomPanelScrollbar;
      interaction_state_.drag_scrollbar_offset =
          Contains(panel_layout.scroll.vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - panel_layout.scroll.vertical_scrollbar->thumb.y
              : panel_layout.scroll.vertical_scrollbar->thumb.h * 0.5f;
      operations_.set_bottom_panel_scroll_row(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*panel_layout.scroll.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              interaction_state_.drag_scrollbar_offset))),
                     0, panel_layout.scroll.max_vertical_scroll),
          line_count, panel_layout.scroll.visible_rows);
      state_.surface.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (HandleMouseCaptureButton(event, true)) {
    return true;
  }

  if (!operations_.bottom_panel_visible() ||
      !Contains(layout.bottom_panel, event.button.x, event.button.y)) {
    return false;
  }

  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    const SDL_FRect panel_content =
        operations_.bottom_panel_content_rect(layout, state_.panel.command_mode);
    if (event.button.button == SDL_BUTTON_RIGHT &&
        Contains(panel_content, event.button.x, event.button.y)) {
      state_.surface.focus = FocusTarget::Panel;
      operations_.open_anchored_menu(
          MenuId::TerminalContext,
          MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                   1.0f));
      return true;
    }
    if (event.button.button == SDL_BUTTON_MIDDLE &&
        Contains(panel_content, event.button.x, event.button.y)) {
      if (const auto text = operations_.read_primary_selection_text(); text.has_value()) {
        operations_.clear_terminal_selection();
        if (!state_.terminal_tabs.empty()) {
          auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
          terminal_tab->follow_tail = true;
          operations_.append_terminal_pending_input(*text);
          terminal_tab->session.PasteText(*text);
        }
      }
      state_.surface.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    const SDL_FRect panel_content =
        operations_.bottom_panel_content_rect(layout, state_.panel.command_mode);
    if (Contains(panel_content, event.button.x, event.button.y)) {
      if (const auto url = operations_.terminal_url_at_point(static_cast<float>(event.button.x),
                                                             static_cast<float>(event.button.y));
          url.has_value() && operations_.open_external_url(*url)) {
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
      auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
      const std::size_t line_count = terminal_tab->session.LineCount();
      const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
      const std::size_t first_row =
          static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
      const auto terminal_lines = terminal_tab->session.SnapshotLineRange(
          first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)));
      if (const auto position =
              operations_.terminal_selection_position_for_point(event.button.x, event.button.y,
                                                                terminal_lines, first_row);
          position.has_value()) {
        terminal_tab->selection_anchor = *position;
        terminal_tab->selection_head = *position;
        terminal_tab->mouse_selecting = true;
        terminal_tab->follow_tail = false;
      } else {
        operations_.clear_terminal_selection();
      }
    } else {
      operations_.clear_terminal_selection();
    }
    state_.surface.focus = FocusTarget::Panel;
  }

  if (state_.panel.content == PanelContentKind::Output) {
    const auto* output_entries = operations_.output_channel_entries();
    if (output_entries != nullptr && !output_entries->empty()) {
      const auto panel_layout =
          operations_.compute_bottom_panel_log_layout(layout, output_entries->size());
      if (Contains(panel_layout.content_rect, event.button.x, event.button.y)) {
        const auto line_index =
            BottomPanelLineIndexAtPoint(panel_layout, static_cast<float>(event.button.y),
                                        output_entries->size());
        if (line_index.has_value() && *line_index < output_entries->size()) {
          const auto parsed = ParseOutputLocationLine((*output_entries)[*line_index]);
          if (parsed.has_value()) {
            std::filesystem::path path = parsed->path;
            if (path.is_relative() && !state_.root.empty()) {
              path = state_.root / path;
            }
            operations_.open_file(path.lexically_normal());
            if (editor::TextViewport* viewport = operations_.active_editor_viewport();
                viewport != nullptr) {
              viewport->MoveCursorTo(parsed->line > 0 ? parsed->line - 1 : 0,
                                     parsed->column > 0 ? parsed->column - 1 : 0);
            }
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

  if (state_.panel.content == PanelContentKind::Debug) {
    DebugExecutionView& exec = state_.debug_execution;
    const std::size_t row_count = exec.PanelRowCount();
    if (row_count > 0) {
      const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, row_count);
      if (Contains(panel_layout.content_rect, event.button.x, event.button.y)) {
        const auto line_index =
            BottomPanelLineIndexAtPoint(panel_layout, static_cast<float>(event.button.y), row_count);
        if (line_index.has_value() && *line_index < row_count) {
          const auto row_ref = exec.PanelRowAt(*line_index);
          if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Session) {
            // Switch the active session: re-project the picked session's stop.
            const int session_id = exec.sessions[row_ref.index].id;
            if (session_id != exec.focused_session_id &&
                operations_.on_debug_session_focus_changed) {
              operations_.on_debug_session_focus_changed(session_id);
            }
            return true;
          }
          if (row_ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
            // Switch to the picked thread: re-resolve its frames + re-focus it.
            const int thread_id = exec.threads[row_ref.index].id;
            if (thread_id != exec.focused_thread_id && operations_.on_debug_thread_focus_changed) {
              operations_.on_debug_thread_focus_changed(thread_id);
            }
            return true;
          }
          // Focus the picked frame; the editor execution-line highlight follows
          // `focused_frame_index`, so selecting a caller moves the highlight too.
          exec.focused_frame_index = row_ref.index;
          const DebugStackFrameView& frame = exec.frames[row_ref.index];
          // Refresh the Variables tree for the newly focused frame.
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
      }
    }
  }

  if (state_.panel.content == PanelContentKind::DebugVariables) {
    DebugVariablesModel& model = state_.debug_variables;
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    if (!rows.empty()) {
      const auto panel_layout =
          operations_.compute_bottom_panel_log_layout(layout, rows.size());
      if (Contains(panel_layout.content_rect, event.button.x, event.button.y)) {
        const auto line_index = BottomPanelLineIndexAtPoint(
            panel_layout, static_cast<float>(event.button.y), rows.size());
        if (line_index.has_value() && *line_index < rows.size()) {
          model.SetSelectedRow(*line_index);
          const DebugVariableRowView& row = rows[*line_index];
          // Double-click a leaf value to edit it; single-click a parent to
          // expand/collapse its subtree (lazy-fetched on first expand).
          if (event.button.clicks >= 2 && row.editable && !row.has_children) {
            if (operations_.begin_debug_variable_edit) {
              operations_.begin_debug_variable_edit(*line_index);
            }
          } else if (event.button.clicks == 1 && row.has_children) {
            if (operations_.toggle_debug_variable_row) {
              operations_.toggle_debug_variable_row(*line_index);
            }
          }
          state_.surface.focus = FocusTarget::Panel;
          return true;
        }
      }
    }
  }

  if (state_.panel.content == PanelContentKind::DebugWatch) {
    DebugWatchModel& model = state_.debug_watch;
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    const auto panel_layout =
        operations_.compute_bottom_panel_log_layout(layout, std::max<std::size_t>(rows.size(), 1));
    if (Contains(panel_layout.content_rect, event.button.x, event.button.y)) {
      const auto line_index = BottomPanelLineIndexAtPoint(
          panel_layout, static_cast<float>(event.button.y), rows.size());
      if (line_index.has_value() && *line_index < rows.size()) {
        model.SetSelectedRow(*line_index);
        const DebugVariableRowView& row = rows[*line_index];
        const std::optional<std::size_t> expr_index = model.ExpressionIndexForRow(*line_index);
        if (event.button.clicks >= 2 && expr_index.has_value()) {
          // Double-click an expression root → edit its expression string.
          if (operations_.edit_debug_watch_expression) {
            operations_.edit_debug_watch_expression(*expr_index);
          }
        } else if (event.button.clicks >= 2 && row.editable && !row.has_children) {
          // Double-click a watched child leaf → inline setVariable edit.
          if (operations_.begin_debug_watch_edit) {
            operations_.begin_debug_watch_edit(*line_index);
          }
        } else if (event.button.clicks == 1 && row.has_children) {
          if (operations_.toggle_debug_watch_row) {
            operations_.toggle_debug_watch_row(*line_index);
          }
        }
      } else if (operations_.add_debug_watch_expression) {
        // Click in the empty area (or empty panel) → prompt for a new expression.
        operations_.add_debug_watch_expression();
      }
      state_.surface.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (state_.panel.content == PanelContentKind::DebugBreakpoints) {
    const DebugBreakpointsModel& model = state_.debug_breakpoints_panel;
    const std::vector<DebugBreakpointRowView>& rows = model.Rows();
    if (!rows.empty()) {
      const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, rows.size());
      if (Contains(panel_layout.content_rect, event.button.x, event.button.y)) {
        const auto line_index = BottomPanelLineIndexAtPoint(
            panel_layout, static_cast<float>(event.button.y), rows.size());
        if (line_index.has_value() && *line_index < rows.size()) {
          const DebugBreakpointRowView& row = rows[*line_index];
          if (row.kind == DebugBreakpointRowView::Kind::ExceptionFilter &&
              operations_.toggle_debug_exception_filter) {
            operations_.toggle_debug_exception_filter(row.filter_id);
          } else if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && !row.path.empty()) {
            // Navigate to the breakpoint's source line.
            operations_.open_file(row.path);
            if (editor::TextViewport* viewport = operations_.active_editor_viewport();
                viewport != nullptr) {
              viewport->MoveCursorTo(row.line, 0);
            }
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
      state_.surface.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (state_.panel.command_mode) {
    state_.surface.focus = FocusTarget::Panel;
  } else if (state_.panel.content != PanelContentKind::None) {
    state_.surface.focus = FocusTarget::Panel;
  }
  return true;
}

bool PanelMouseCoordinator::HandleButtonUp(const SDL_Event& event) {
  if (HandleMouseCaptureButton(event, false)) {
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  if (interaction_state_.drag_target == DragTarget::BottomPanelDivider ||
      interaction_state_.drag_target == DragTarget::BottomPanelScrollbar) {
    interaction_state_.drag_target = DragTarget::None;
    return true;
  }

  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
    if (terminal_tab->mouse_selecting) {
      terminal_tab->mouse_selecting = false;
      operations_.sync_primary_selection_with_terminal_selection();
      return true;
    }
  }

  return false;
}

bool PanelMouseCoordinator::HandleDrag(const SDL_Event& event,
                                       const WorkspaceLayout& layout) {
  if (interaction_state_.drag_target == DragTarget::BottomPanelDivider) {
    const auto window_rect = operations_.current_window_rect();
    if (!window_rect.has_value()) {
      return false;
    }
    const float desired_height = window_rect->h - static_cast<float>(event.motion.y);
    state_.panel.height = operations_.clamp_bottom_panel_height(desired_height, window_rect->h);
    return true;
  }

  if (interaction_state_.drag_target != DragTarget::BottomPanelScrollbar ||
      !operations_.bottom_panel_visible()) {
    return false;
  }

  const std::size_t line_count = operations_.bottom_panel_line_count();
  const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
  if (!panel_layout.scroll.vertical_scrollbar.has_value()) {
    interaction_state_.drag_target = DragTarget::None;
    return false;
  }
  operations_.set_bottom_panel_scroll_row(
      std::clamp(static_cast<int>(std::lround(ScrollUnitsForPointer(
                     *panel_layout.scroll.vertical_scrollbar, static_cast<float>(event.motion.y),
                     interaction_state_.drag_scrollbar_offset))),
                 0, panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  state_.surface.focus = FocusTarget::Panel;
  return true;
}

bool PanelMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
    const bool buttons_down =
        (event.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
    if (terminal_tab->session.WantsMouseMotionCapture(buttons_down)) {
      if (const auto viewport_position =
              operations_.terminal_viewport_position_for_point(event.motion.x, event.motion.y);
          viewport_position.has_value()) {
        terminal::TerminalSession::MouseButton button =
            terminal::TerminalSession::MouseButton::None;
        if ((event.motion.state & SDL_BUTTON_LMASK) != 0) {
          button = terminal::TerminalSession::MouseButton::Left;
        } else if ((event.motion.state & SDL_BUTTON_MMASK) != 0) {
          button = terminal::TerminalSession::MouseButton::Middle;
        } else if ((event.motion.state & SDL_BUTTON_RMASK) != 0) {
          button = terminal::TerminalSession::MouseButton::Right;
        }
        operations_.clear_terminal_selection();
        terminal_tab->session.SendMouseMotion(button, viewport_position->row,
                                              viewport_position->column, SDL_GetModState());
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
    if (terminal_tab->mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0 &&
        operations_.bottom_panel_visible()) {
      const auto layout_state = operations_.current_workspace_layout();
      if (!layout_state.has_value()) {
        return false;
      }
      const WorkspaceLayout layout = *layout_state;
      if (!Contains(layout.bottom_panel, event.motion.x, event.motion.y)) {
        return false;
      }

      const std::size_t line_count = terminal_tab->session.LineCount();
      const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
      const std::size_t first_row =
          static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
      const auto terminal_lines = terminal_tab->session.SnapshotLineRange(
          first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)));
      if (const auto position = operations_.terminal_selection_position_for_point(
              event.motion.x, event.motion.y, terminal_lines, first_row);
          position.has_value()) {
        terminal_tab->selection_head = *position;
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  return false;
}

bool PanelMouseCoordinator::HandleWheel(const SDL_Event& event,
                                        const WorkspaceLayout& layout,
                                        int vertical_ticks) {
  if (state_.panel.content == PanelContentKind::Terminal && !state_.terminal_tabs.empty()) {
    auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
    if (terminal_tab->session.WantsMouseCapture()) {
      if (const auto viewport_position =
              operations_.terminal_viewport_position_for_point(event.wheel.mouse_x, event.wheel.mouse_y);
          viewport_position.has_value()) {
        const auto button = vertical_ticks > 0 ? terminal::TerminalSession::MouseButton::WheelUp
                                               : terminal::TerminalSession::MouseButton::WheelDown;
        const int step_count = std::abs(vertical_ticks);
        operations_.clear_terminal_selection();
        for (int i = 0; i < step_count; ++i) {
          terminal_tab->session.SendMouseButton(button, true, viewport_position->row,
                                                viewport_position->column, SDL_GetModState());
        }
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (!operations_.bottom_panel_visible() ||
      !Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const std::size_t line_count = operations_.bottom_panel_line_count();
  const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
  operations_.set_bottom_panel_scroll_row(
      std::clamp(panel_layout.scroll.vertical_scroll - vertical_ticks, 0,
                 panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  state_.surface.focus = FocusTarget::Panel;
  return true;
}

bool PanelMouseCoordinator::HandleMouseCaptureButton(const SDL_Event& event, bool pressed) {
  if (state_.panel.content != PanelContentKind::Terminal || state_.terminal_tabs.empty()) {
    return false;
  }

  auto* terminal_tab = state_.terminal_tabs[state_.active_terminal_tab_index].get();
  const auto viewport_position =
      operations_.terminal_viewport_position_for_point(event.button.x, event.button.y);
  const auto mouse_button = operations_.terminal_mouse_button_for_sdl(event.button.button);
  if (terminal_tab == nullptr || !viewport_position.has_value() ||
      mouse_button == terminal::TerminalSession::MouseButton::None ||
      !terminal_tab->session.WantsMouseCapture()) {
    return false;
  }

  operations_.clear_terminal_selection();
  terminal_tab->session.SendMouseButton(mouse_button, pressed, viewport_position->row,
                                        viewport_position->column, SDL_GetModState());
  state_.surface.focus = FocusTarget::Panel;
  return true;
}

PanelMouseCoordinator WorkspaceShell::MakePanelMouseCoordinator() {
  auto terminal_panel = MakeTerminalPanelService();
  return PanelMouseCoordinator(
      context_.current_project_state, context_.menu_state, context_.interaction_state,
      PanelMouseCoordinator::Operations{
          .bottom_panel_visible = [this]() { return BottomPanelVisible(); },
          .bottom_panel_resize_handle_rect =
              [this](const WorkspaceLayout& layout) { return BottomPanelResizeHitRect(layout); },
          .compute_bottom_panel_log_layout =
              [this](const WorkspaceLayout& layout, std::size_t line_count) {
                return ComputeBottomPanelLogLayout(layout, line_count);
              },
          .bottom_panel_line_count =
              [this]() {
                if (BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr) {
                  return ActiveTerminalTab()->session.LineCount();
                }
                if (BottomPanelShowsOutput()) {
                  if (const auto* entries =
                          OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
                      entries != nullptr) {
                    return entries->size();
                  }
                  return std::size_t{0};
                }
                const ProjectWorkspaceState& state = context_.current_project_state;
                if (state.panel.content == PanelContentKind::Debug) {
                  return state.debug_execution.PanelRowCount();
                }
                if (state.panel.content == PanelContentKind::DebugVariables) {
                  return state.debug_variables.Rows().size();
                }
                if (state.panel.content == PanelContentKind::DebugWatch) {
                  return state.debug_watch.Rows().size();
                }
                if (state.panel.content == PanelContentKind::DebugBreakpoints) {
                  return state.debug_breakpoints_panel.RowCount();
                }
                return std::size_t{0};
              },
          .set_bottom_panel_scroll_row =
              [this](int row, std::size_t count, int visible_rows) {
                SetBottomPanelScrollRow(row, count, visible_rows);
              },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& rect) { MakeMenuCoordinator().OpenAnchoredMenu(id, rect); },
          .bottom_panel_content_rect =
              [this](const WorkspaceLayout& layout, bool command_mode) {
                return BottomPanelContentRect(layout, command_mode);
              },
          .read_primary_selection_text =
              [terminal_panel]() mutable { return terminal_panel.ReadPrimarySelectionText(); },
          .clear_terminal_selection =
              [terminal_panel]() mutable { terminal_panel.ClearTerminalSelection(); },
          .append_terminal_pending_input =
              [terminal_panel](std::string_view input) mutable {
                terminal_panel.AppendTerminalPendingInput(input);
              },
          .terminal_url_at_point =
              [terminal_panel](float x, float y) mutable {
                return terminal_panel.TerminalUrlAtPoint(x, y);
              },
          .open_external_url =
              [terminal_panel](std::string_view url) mutable {
                return terminal_panel.OpenExternalUrl(url);
              },
          .terminal_selection_position_for_point =
              [this](int x,
                     int y,
                     const std::vector<terminal::TerminalLine>& lines,
                     std::size_t first_row) {
                return TerminalSelectionPositionForPoint(x, y, lines, first_row);
              },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .terminal_viewport_position_for_point =
              [this](int x, int y) { return TerminalViewportPositionForPoint(x, y); },
          .terminal_mouse_button_for_sdl =
              [this](Uint8 button) { return TerminalMouseButtonForSdl(button); },
          .output_channel_entries =
              [this]() {
                return OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
              },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .clamp_bottom_panel_height =
              [](float desired_height, float window_height) {
                return ClampBottomPanelHeight(desired_height, window_height);
              },
          .sync_primary_selection_with_terminal_selection =
              [terminal_panel]() mutable {
                terminal_panel.SyncPrimarySelectionWithTerminalSelection();
              },
          .on_debug_frame_focus_changed =
              [this](int frame_id) { debug_service_.FocusFrame(frame_id); },
          .on_debug_thread_focus_changed =
              [this](int thread_id) { debug_service_.FocusThread(thread_id); },
          .on_debug_session_focus_changed =
              [this](int session_id) { debug_service_.FocusSession(session_id); },
          .toggle_debug_variable_row =
              [this](std::size_t row) { debug_service_.ToggleVariableRow(row); },
          .begin_debug_variable_edit =
              [this](std::size_t row) { debug_service_.BeginVariableEdit(row); },
          .toggle_debug_watch_row =
              [this](std::size_t row) { debug_service_.ToggleWatchRow(row); },
          .begin_debug_watch_edit =
              [this](std::size_t row) { debug_service_.BeginWatchEdit(row); },
          .add_debug_watch_expression = [this]() { OpenWatchExpressionPrompt(std::nullopt); },
          .edit_debug_watch_expression =
              [this](std::size_t index) { OpenWatchExpressionPrompt(index); },
          .toggle_debug_exception_filter =
              [this](const std::string& filter_id) {
                debug_service_.ToggleExceptionFilter(filter_id);
              },
      });
}

}  // namespace microide::workspace
