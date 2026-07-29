#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "workspace/DebugPaneMouseCoordinator.h"
#include "workspace/DebugPaneRegistry.h"
#include "workspace/DebugPaneService.h"
#include "workspace/DebugService.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceLayout.h"

// Geometry + scroll helpers for the right-side debug pane. These read project
// state, so they deliberately live OUTSIDE the lint-covered render TU
// (DebugPaneRender.cpp); that file consumes only the view model and calls these
// members. Mirrors the bottom-panel ComputeBottomPanelLogLayout / SetBottomPanelScrollRow
// split (those live in WorkspaceShellTerminal.cpp).

namespace microide::workspace {

namespace {

// Header band reserved at the top of the pane for the mode-row button switcher.
constexpr float kDebugPaneModeRowTop = 4.0f;
constexpr float kDebugPaneModeRowHeight = 20.0f;
constexpr float kDebugPaneHeaderBandHeight = kDebugPaneModeRowTop + kDebugPaneModeRowHeight + 6.0f;
constexpr float kDebugPaneInset = 10.0f;
constexpr float kDebugPaneModeTabGap = 4.0f;
constexpr float kDebugPaneModeIconSlot = 16.0f;
constexpr float kDebugPaneModeIconLabelGap = 5.0f;
constexpr float kDebugPaneModeLabelPadding = 10.0f;
constexpr float kDebugPaneTextInset = 12.0f;
constexpr float kDebugPaneTextTopInset = 8.0f;
constexpr float kDebugPaneScrollbarTextReserve = 16.0f;

SDL_FRect DebugPaneContentRect(const SDL_FRect& pane) {
  return MakeRect(pane.x, pane.y + kDebugPaneHeaderBandHeight, pane.w,
                  std::max(0.0f, pane.h - kDebugPaneHeaderBandHeight));
}

}  // namespace

DebugPaneRowHit DebugPaneRowAtPoint(const SDL_FRect& content_rect, float text_y, float line_height,
                                    int visible_rows, int vertical_scroll, std::size_t line_count,
                                    float x, float y) {
  DebugPaneRowHit hit;
  if (line_height <= 0.0f || !Contains(content_rect, x, y)) {
    return hit;  // row_index = -1, in_content = false
  }
  hit.in_content = true;
  // The top text inset (content_rect.y .. text_y) belongs to the first visible row
  // rather than rejecting the click — this is the dead-zone fix.
  int relative = 0;
  if (y > text_y) {
    relative = static_cast<int>(std::floor((y - text_y) / line_height));
  }
  if (relative < 0 || relative >= visible_rows) {
    return hit;  // inside content but below the last visible row
  }
  const int absolute = vertical_scroll + relative;
  if (absolute < 0 || static_cast<std::size_t>(absolute) >= line_count) {
    return hit;
  }
  hit.row_index = absolute;
  return hit;
}

DebugPaneModeRowLayout WorkspaceShell::DebugPaneModeRow(const SDL_FRect& pane_rect) const {
  DebugPaneModeRowLayout layout;
  if (pane_rect.w <= 0.0f || pane_rect.h <= 0.0f) {
    return layout;
  }
  const float left = pane_rect.x + kDebugPaneInset;
  const float top = pane_rect.y + kDebugPaneModeRowTop;
  const float right = pane_rect.x + pane_rect.w - kDebugPaneInset;
  layout.row_rect = MakeRect(left, top, std::max(0.0f, right - left), kDebugPaneModeRowHeight);
  if (layout.row_rect.w <= 0.0f) {
    return layout;
  }

  for (const DebugPaneSurfaceSpec& spec : BuiltinDebugPaneSurfaceSpecs()) {
    if (layout.tab_count < static_cast<int>(layout.tabs.size())) {
      layout.tabs[static_cast<std::size_t>(layout.tab_count++)] =
          DebugPaneModeTab{.id = spec.id, .label = spec.label, .mode = spec.mode, .rect = {}};
    }
  }
  if (layout.tab_count == 0) {
    return layout;
  }

  const float gaps = kDebugPaneModeTabGap * static_cast<float>(layout.tab_count - 1);
  float labelled_total = gaps;
  for (int i = 0; i < layout.tab_count; ++i) {
    labelled_total += kDebugPaneModeIconSlot + kDebugPaneModeIconLabelGap +
                      text_renderer_.MeasureWidth(layout.tabs[static_cast<std::size_t>(i)].label) +
                      kDebugPaneModeLabelPadding;
  }
  layout.icon_only = labelled_total > layout.row_rect.w;

  float x = layout.row_rect.x;
  for (int i = 0; i < layout.tab_count; ++i) {
    float w = 0.0f;
    if (layout.icon_only) {
      w = std::max(kDebugPaneModeIconSlot,
                   (layout.row_rect.w - gaps) / static_cast<float>(layout.tab_count));
    } else {
      w = kDebugPaneModeIconSlot + kDebugPaneModeIconLabelGap +
          text_renderer_.MeasureWidth(layout.tabs[static_cast<std::size_t>(i)].label) +
          kDebugPaneModeLabelPadding;
    }
    layout.tabs[static_cast<std::size_t>(i)].rect =
        MakeRect(x, layout.row_rect.y, w, layout.row_rect.h);
    x += w + kDebugPaneModeTabGap;
  }
  return layout;
}

std::size_t WorkspaceShell::DebugPaneActiveRowCount() const {
  const ProjectWorkspaceState& ps = context_.current_project_state;
  switch (ps.debug_pane.mode) {
    case DebugPaneMode::CallStack:
      return ps.debug_execution.PanelRowCount();
    case DebugPaneMode::Variables:
      return ps.debug_variables.Rows().size();
    case DebugPaneMode::Watch:
      return ps.debug_watch.Rows().size();
    case DebugPaneMode::Breakpoints:
      return ps.debug_breakpoints_panel.RowCount();
  }
  return 0;
}

void WorkspaceShell::RevealDebugPaneSelection() {
  // Keyboard navigation moved the selection without touching the scroll, so
  // arrowing past the last visible row walked the highlight off screen — the same
  // failure the git sidebar's visible-row walk was written to avoid. Every other
  // list in the shell reveals. Each mode names its selection differently, which is
  // the whole of the switch below.
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const ProjectWorkspaceState& ps = context_.current_project_state;
  int selected = -1;
  switch (ps.debug_pane.mode) {
    case DebugPaneMode::Variables:
      selected = static_cast<int>(ps.debug_variables.SelectedRow());
      break;
    case DebugPaneMode::Watch:
      selected = static_cast<int>(ps.debug_watch.SelectedRow());
      break;
    case DebugPaneMode::CallStack:
      // The Call Stack's selection is the focused session/thread/frame, which is
      // also what the render highlights — there is no separate selection to track.
      selected = static_cast<int>(ps.debug_execution.FocusedPanelRow());
      break;
    case DebugPaneMode::Breakpoints:
      selected = ps.debug_pane.breakpoints_selected_row;
      break;
    default:
      return;
  }
  const std::size_t line_count = DebugPaneActiveRowCount();
  const LogSurfaceLayout panel_layout = ComputeDebugPaneListLayout(*layout_state, line_count);
  SetDebugPaneScrollRow(RevealedScrollRow(panel_layout.scroll.vertical_scroll,
                                          panel_layout.scroll.visible_rows, selected),
                        line_count, panel_layout.scroll.visible_rows);
}

void WorkspaceShell::FocusDebugCallStackRow(std::size_t row) {
  MakeDebugPaneMouseCoordinator().ActivateCallStackRow(row, /*move_focus_to_editor=*/false);
}

void WorkspaceShell::ActivateDebugBreakpointRow(std::size_t row, bool toggle) {
  MakeDebugPaneMouseCoordinator().ActivateBreakpointRow(row, toggle,
                                                        /*move_focus_to_editor=*/false);
}

bool WorkspaceShell::DebugPaneRowIsActionable(std::size_t row) const {
  const ProjectWorkspaceState& ps = context_.current_project_state;
  switch (ps.debug_pane.mode) {
    case DebugPaneMode::CallStack:
      // Every panel row (session / thread / frame) focuses or navigates.
      return row < ps.debug_execution.PanelRowCount();
    case DebugPaneMode::Variables: {
      const std::vector<DebugVariableRowView>& rows = ps.debug_variables.Rows();
      if (row >= rows.size()) {
        return false;
      }
      const DebugVariableRowView& r = rows[row];
      return !r.is_placeholder && (r.has_children || r.is_show_more || r.editable);
    }
    case DebugPaneMode::Watch: {
      const std::vector<DebugVariableRowView>& rows = ps.debug_watch.Rows();
      if (row >= rows.size()) {
        return false;
      }
      const DebugVariableRowView& r = rows[row];
      return !r.is_placeholder &&
             (r.has_children || r.is_show_more || r.editable ||
              ps.debug_watch.ExpressionIndexForRow(row).has_value());
    }
    case DebugPaneMode::Breakpoints: {
      const std::vector<DebugBreakpointRowView>& rows = ps.debug_breakpoints_panel.Rows();
      if (row >= rows.size()) {
        return false;
      }
      const DebugBreakpointRowView& r = rows[row];
      if (r.kind == DebugBreakpointRowView::Kind::ExceptionFilter) {
        return true;
      }
      if (r.kind == DebugBreakpointRowView::Kind::Breakpoint) {
        return !r.path.empty();
      }
      return false;  // section header
    }
  }
  return false;
}

int WorkspaceShell::DebugPaneScrollRow(std::size_t line_count, int visible_rows) const {
  const DebugPaneState& pane = context_.current_project_state.debug_pane;
  int requested = 0;
  switch (pane.mode) {
    case DebugPaneMode::CallStack:
      requested = pane.call_stack_scroll_row;
      break;
    case DebugPaneMode::Variables:
      requested = pane.variables_scroll_row;
      break;
    case DebugPaneMode::Watch:
      requested = pane.watch_scroll_row;
      break;
    case DebugPaneMode::Breakpoints:
      requested = pane.breakpoints_scroll_row;
      break;
  }
  return ClampScrollRowToContent(requested, line_count, visible_rows);
}

void WorkspaceShell::SetDebugPaneScrollRow(int scroll_row, std::size_t line_count,
                                           int visible_rows) {
  const int clamped = ClampScrollRowToContent(scroll_row, line_count, visible_rows);
  DebugPaneState& pane = context_.current_project_state.debug_pane;
  switch (pane.mode) {
    case DebugPaneMode::CallStack:
      pane.call_stack_scroll_row = clamped;
      break;
    case DebugPaneMode::Variables:
      pane.variables_scroll_row = clamped;
      break;
    case DebugPaneMode::Watch:
      pane.watch_scroll_row = clamped;
      break;
    case DebugPaneMode::Breakpoints:
      pane.breakpoints_scroll_row = clamped;
      break;
  }
}

DebugPaneService WorkspaceShell::MakeDebugPaneService() {
  return DebugPaneService(context_.current_project_state,
                          DebugPaneService::Operations{
                              .request_redraw = [this]() { RequestWindowRedraw(); },
                              .mark_layout_dirty = [this]() { MarkLayoutDirty(); },
                          });
}

void WorkspaceShell::ShowDebugPaneMode(DebugPaneMode mode) {
  MakeDebugPaneService().ShowMode(mode);
}

void WorkspaceShell::ToggleDebugPane() { MakeDebugPaneService().Toggle(); }

void WorkspaceShell::CloseDebugPane() { MakeDebugPaneService().Close(); }

void WorkspaceShell::OpenDebugPaneOnStop() { MakeDebugPaneService().OpenOnStop(); }

DebugPaneMouseCoordinator WorkspaceShell::MakeDebugPaneMouseCoordinator() {
  return DebugPaneMouseCoordinator(
      context_.current_project_state, context_.interaction_state,
      DebugPaneMouseCoordinator::Operations{
          .compute_debug_pane_list_layout =
              [this](const WorkspaceLayout& layout, std::size_t line_count) {
                return ComputeDebugPaneListLayout(layout, line_count);
              },
          .debug_pane_mode_row =
              [this](const SDL_FRect& pane_rect) { return DebugPaneModeRow(pane_rect); },
          .debug_pane_active_row_count = [this]() { return DebugPaneActiveRowCount(); },
          .set_debug_pane_scroll_row =
              [this](int row, std::size_t count, int visible_rows) {
                SetDebugPaneScrollRow(row, count, visible_rows);
              },
          .show_debug_pane_mode = [this](DebugPaneMode mode) { ShowDebugPaneMode(mode); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
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
          .toggle_debug_breakpoint_enabled =
              [this](const std::filesystem::path& path, std::size_t line) {
                if (context_.current_project_state.breakpoint_store.ToggleEnabled(path, line)) {
                  // Re-send the file's breakpoints (a disabled one drops off the
                  // adapter) and rebuild the panel + gutter.
                  ResendBreakpointsForFile(path);
                  RequestFocusedEditorRedraw();
                }
              },
          .toggle_debug_function_breakpoint_enabled =
              [this](std::size_t index) {
                debug_service_.ToggleFunctionBreakpointEnabled(index);
              },
          .open_debug_value_context_menu =
              [this](const SDL_FRect& anchor) {
                MakeMenuCoordinator().OpenTreeContextMenu(TreeContextTargetKind::DebugValueRow, {},
                                                          anchor);
              },
          .open_breakpoint_context_menu =
              [this](const std::filesystem::path& path, std::size_t line,
                     const SDL_FRect& anchor) { OpenBreakpointContextMenu(path, line, anchor); },
      });
}

WorkspaceShell::LogSurfaceLayout WorkspaceShell::ComputeDebugPaneListLayout(
    const WorkspaceLayout& layout, std::size_t line_count) const {
  LogSurfaceLayout panel_layout;
  panel_layout.content_rect = DebugPaneContentRect(layout.right_pane);
  panel_layout.text_x = panel_layout.content_rect.x + kDebugPaneTextInset;
  panel_layout.text_y = panel_layout.content_rect.y + kDebugPaneTextTopInset;
  panel_layout.line_height = text_renderer_.LineHeight();

  int visible_rows = 1;
  if (panel_layout.line_height > 0.0f) {
    visible_rows = std::max(1, static_cast<int>((panel_layout.content_rect.h -
                                                 kDebugPaneTextTopInset) /
                                                panel_layout.line_height));
  }
  const int scroll_row = DebugPaneScrollRow(line_count, visible_rows);
  panel_layout.scroll =
      ComputeScrollSurfaceLayout(panel_layout.content_rect, line_count, visible_rows, scroll_row);
  panel_layout.text_width =
      std::max(0.0f, panel_layout.content_rect.w - kDebugPaneTextInset * 2.0f -
                         (panel_layout.scroll.show_vertical ? kDebugPaneScrollbarTextReserve
                                                            : 0.0f));
  return panel_layout;
}

}  // namespace microide::workspace
