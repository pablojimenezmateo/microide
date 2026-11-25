#include <memory>

#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"

#include "render/SdlConvert.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

#include "workspace/coordinators/SelectionAutoscroll.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/ListSelection.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/shell/ShellGlueCache.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceOutputReference.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

std::optional<std::size_t> BottomPanelLineIndexAtPoint(
    const WorkspaceShell::LogSurfaceLayout& panel_layout,
    float y,
    std::size_t line_count) {
  return BottomPanelLineIndexAtY(panel_layout.text_y, panel_layout.line_height,
                                 panel_layout.scroll.visible_rows,
                                 panel_layout.scroll.vertical_scroll, y, line_count);
}

}

PanelMouseCoordinator::PanelMouseCoordinator(ProjectWorkspaceState& state,
                                             InteractionState& interaction_state,
                                             Services services,
                                             Operations operations)
    : state_(state),
      interaction_state_(interaction_state),
      services_(services),
      operations_(std::move(operations)) {}

bool PanelMouseCoordinator::HandleResizeButtonDown(const SDL_Event& event,
                                                   const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !operations_.bottom_panel_visible() ||
      !Contains(operations_.bottom_panel_resize_handle_rect(layout), event.button.x,
                event.button.y)) {
    return false;
  }
  if (event.button.clicks >= 2) {
    if (const auto window_rect = operations_.current_window_rect(); window_rect.has_value()) {
      state_.panel.height = operations_.clamp_bottom_panel_height(
          kWorkspaceDefaultBottomPanelHeight, window_rect->h);
    }
    interaction_state_.drag_target = DragTarget::None;
    return true;
  }
  interaction_state_.drag_target = DragTarget::BottomPanelDivider;
  return true;
}

bool PanelMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                             const WorkspaceLayout& layout) {
  if (operations_.bottom_panel_visible() && state_.panel.content == PanelContentKind::Terminal &&
      Contains(operations_.bottom_panel_content_rect(layout), event.button.x, event.button.y)) {
    const float x = static_cast<float>(event.button.x);
    const float y = static_cast<float>(event.button.y);
    if (event.button.button == SDL_BUTTON_LEFT) {
      if (const auto divider = services_.terminal_panel.DividerAt(layout, x, y);
          divider.has_value()) {
        if (event.button.clicks >= 2) {
          services_.terminal_panel.ResetActiveTabDivider(divider->boundary);
          interaction_state_.drag_target = DragTarget::None;
          return true;
        }
        interaction_state_.drag_target = DragTarget::TerminalPaneDivider;
        interaction_state_.drag_terminal_pane_boundary = static_cast<std::uint8_t>(divider->boundary);
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
    services_.terminal_panel.ActivatePaneAt(layout, x, y);
  }

  if (event.button.button == SDL_BUTTON_LEFT && operations_.bottom_panel_visible()) {
    const std::size_t line_count = operations_.bottom_panel_line_count();
    const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
    if (panel_layout.scroll.vertical_scrollbar.has_value() &&
        Contains(panel_layout.scroll.vertical_scrollbar->track, event.button.x, event.button.y)) {
      interaction_state_.drag_target = DragTarget::BottomPanelScrollbar;
      interaction_state_.drag_scrollbar_offset =
          ScrollbarGrabOffset(*panel_layout.scroll.vertical_scrollbar,
                              static_cast<float>(event.button.y), true);
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

  if (event.button.button == SDL_BUTTON_LEFT && operations_.bottom_panel_visible() &&
      state_.panel.content == PanelContentKind::PluginSurface) {
    if (const editor::SurfaceContent* content = operations_.active_plugin_surface();
        content != nullptr && content->has_body()) {
      const SDL_FRect body = operations_.bottom_panel_content_rect(layout);
      if (const auto geometry = MakeVerticalScrollbarGeometry(
              body, PluginSurfacePreviewContentHeight(*content), body.h,
              static_cast<float>(state_.panel.surface_scroll_y));
          geometry.has_value() && Contains(geometry->track, event.button.x, event.button.y)) {
        interaction_state_.drag_target = DragTarget::BottomPanelScrollbar;
        interaction_state_.drag_scrollbar_offset =
            ScrollbarGrabOffset(*geometry, static_cast<float>(event.button.y), true);
        state_.panel.surface_scroll_y = std::clamp(
            static_cast<int>(std::lround(
                ScrollUnitsForPointer(*geometry, static_cast<float>(event.button.y),
                                      interaction_state_.drag_scrollbar_offset))),
            0, MaxPluginSurfacePreviewScroll(*content, body.h));
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
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
    const SDL_FRect panel_content = operations_.bottom_panel_content_rect(layout);
    if (event.button.button == SDL_BUTTON_RIGHT &&
        Contains(panel_content, event.button.x, event.button.y)) {
      state_.surface.focus = FocusTarget::Panel;
      services_.menus.OpenAnchoredMenu(MenuId::TerminalContext, MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                   1.0f));
      return true;
    }
    if (event.button.button == SDL_BUTTON_MIDDLE &&
        Contains(panel_content, event.button.x, event.button.y)) {
      if (const auto text = services_.terminal_panel.ReadPrimarySelectionText(); text.has_value()) {
        services_.terminal_panel.ClearTerminalSelection();
        if (auto* terminal_tab = state_.active_terminal_pane()) {
          terminal_tab->follow_tail = true;
          services_.terminal_panel.AppendTerminalPendingInput(*text);
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

  if (operations_.terminal_find_mouse_down &&
      operations_.terminal_find_mouse_down(static_cast<float>(event.button.x),
                                           static_cast<float>(event.button.y))) {
    state_.surface.focus = FocusTarget::Panel;
    return true;
  }

  if (auto* terminal_tab = ActivePanelTerminalPane()) {
    const SDL_FRect panel_content = operations_.bottom_panel_content_rect(layout);
    if (Contains(panel_content, event.button.x, event.button.y)) {
      if (const auto link = services_.terminal_panel.TerminalLinkAtPoint(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
          link.has_value()) {
        const std::string* url = &link->target;
        if (url->starts_with("file://")) {
          std::string_view rest(*url);
          rest.remove_prefix(7);
          std::size_t line = 0;
          std::size_t column = 0;
          if (const std::size_t fragment = rest.rfind("#L"); fragment != std::string_view::npos) {
            std::string_view position = rest.substr(fragment + 2);
            rest = rest.substr(0, fragment);
            const std::size_t colon = position.find(':');
            line = util::ParseSize(position.substr(0, colon)).value_or(0);
            if (colon != std::string_view::npos) {
              column = util::ParseSize(position.substr(colon + 1)).value_or(0);
            }
          }
          const std::filesystem::path path(rest);
          std::error_code error;
          if (!path.empty() && std::filesystem::is_regular_file(path, error) && !error) {
            operations_.open_file(path);
            if (editor::TextViewport* viewport = operations_.active_editor_viewport();
                viewport != nullptr && line > 0) {
              const std::size_t target_line = std::min(line - 1, viewport->line_count() - 1);
              const std::size_t byte_column =
                  column > 0 ? util::Utf8ByteOffsetForCodepointCount(
                                   viewport->lines().LineView(target_line), column - 1)
                             : 0;
              viewport->JumpCursorTo(target_line, byte_column);
            }
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
        if (operations_.open_external_url(*url)) {
          state_.surface.focus = FocusTarget::Panel;
          return true;
        }
      }
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
        const terminal::TerminalLine* line =
            position->row >= first_row && position->row - first_row < terminal_lines.size()
                ? &terminal_lines[position->row - first_row]
                : nullptr;
        const bool shift = (interaction_state_.key_modifiers & SDL_KMOD_SHIFT) != 0;
        std::optional<TerminalSelectionBounds> expanded;
        if (event.button.clicks >= 2 && line != nullptr) {
          expanded = event.button.clicks == 2
                         ? TerminalWordBoundsAt(*line, position->row, position->column)
                         : TerminalLineBoundsAt(*line, position->row);
        }
        if (shift && event.button.clicks == 1 && terminal_tab->selection_anchor.has_value() &&
            line != nullptr && !terminal_tab->session.WantsMouseCapture()) {
          const TerminalSelectionEnds ends = ExtendTerminalSelection(
              terminal_tab->selection_unit, terminal_tab->selection_origin, *line, *position);
          terminal_tab->selection_anchor = ends.anchor;
          terminal_tab->selection_head = ends.head;
        } else if (expanded.has_value()) {
          terminal_tab->selection_unit =
              event.button.clicks == 2 ? TerminalSelectionUnit::Word : TerminalSelectionUnit::Line;
          terminal_tab->selection_origin = *expanded;
          terminal_tab->selection_anchor = expanded->start;
          terminal_tab->selection_head = expanded->end;
        } else {
          terminal_tab->selection_unit = TerminalSelectionUnit::Cell;
          terminal_tab->selection_origin = TerminalSelectionBounds{.start = *position, .end = *position};
          terminal_tab->selection_anchor = *position;
          terminal_tab->selection_head = *position;
        }
        terminal_tab->mouse_selecting = true;
        terminal_tab->follow_tail = false;
      } else {
        services_.terminal_panel.ClearTerminalSelection();
      }
    } else {
      services_.terminal_panel.ClearTerminalSelection();
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
          const auto parsed = ParseOutputReference((*output_entries)[*line_index]);
          if (parsed.has_value()) {
            std::filesystem::path path = parsed->path;
            if (path.is_relative() && !state_.root.empty()) {
              path = state_.root / path;
            }
            operations_.open_file(path.lexically_normal());
            if (editor::TextViewport* viewport = operations_.active_editor_viewport();
                viewport != nullptr && viewport->line_count() > 0) {
              const std::size_t line =
                  std::min(parsed->line > 0 ? parsed->line - 1 : 0, viewport->line_count() - 1);
              const std::size_t byte_column =
                  parsed->column > 0
                      ? util::Utf8ByteOffsetForCodepointCount(viewport->lines().LineView(line),
                                                              parsed->column - 1)
                      : 0;
              viewport->JumpCursorTo(line, byte_column);
            }
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

  if (state_.panel.content == PanelContentKind::PluginSurface) {
    state_.surface.focus = FocusTarget::Panel;
    if (const editor::SurfaceContent* content = operations_.active_plugin_surface();
        content != nullptr && content->has_body()) {
      const SDL_FRect body = operations_.bottom_panel_content_rect(layout);
      if (Contains(body, event.button.x, event.button.y)) {
        if (const editor::SurfaceHitRegion* region = FindPluginSurfacePreviewHitRegion(
                *content, body, static_cast<float>(state_.panel.surface_scroll_y),
                static_cast<float>(event.button.x), static_cast<float>(event.button.y))) {
          operations_.execute_command(region->command);
        }
      }
    }
    return true;
  }

  if (state_.panel.content != PanelContentKind::None) {
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

  if (auto* terminal_tab = ActivePanelTerminalPane()) {
    if (terminal_tab->mouse_selecting) {
      terminal_tab->mouse_selecting = false;
      selection_autoscroll::Disarm(interaction_state_);
      services_.terminal_panel.SyncPrimarySelectionWithTerminalSelection();
      return true;
    }
  }

  return false;
}

bool PanelMouseCoordinator::HandleDrag(const SDL_Event& event,
                                       const WorkspaceLayout& layout) {
  if (interaction_state_.drag_target == DragTarget::TerminalPaneDivider) {
    const std::size_t boundary = interaction_state_.drag_terminal_pane_boundary;
    for (const TerminalPaneDividerRect& divider : services_.terminal_panel.PaneRects(layout).dividers) {
      if (divider.boundary != boundary || divider.pair_extent <= 0.0f) {
        continue;
      }
      services_.terminal_panel.ResizeActiveTabDivider(boundary, (static_cast<float>(event.motion.x) - divider.pair_start) / divider.pair_extent);
      return true;
    }
    return false;
  }

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

  if (state_.panel.content == PanelContentKind::PluginSurface) {
    const editor::SurfaceContent* content = operations_.active_plugin_surface();
    if (content == nullptr || !content->has_body()) {
      interaction_state_.drag_target = DragTarget::None;
      return false;
    }
    const SDL_FRect body = operations_.bottom_panel_content_rect(layout);
    const auto geometry = MakeVerticalScrollbarGeometry(
        body, PluginSurfacePreviewContentHeight(*content), body.h,
        static_cast<float>(state_.panel.surface_scroll_y));
    if (!geometry.has_value()) {
      interaction_state_.drag_target = DragTarget::None;
      return false;
    }
    state_.panel.surface_scroll_y = std::clamp(
        static_cast<int>(std::lround(
            ScrollUnitsForPointer(*geometry, static_cast<float>(event.motion.y),
                                  interaction_state_.drag_scrollbar_offset))),
        0, MaxPluginSurfacePreviewScroll(*content, body.h));
    state_.surface.focus = FocusTarget::Panel;
    return true;
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
  if (auto* terminal_tab = ActivePanelTerminalPane()) {
    const bool buttons_down =
        (event.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
    if (ProgramOwnsPointer(*terminal_tab) &&
        terminal_tab->session.WantsMouseMotionCapture(buttons_down)) {
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
        services_.terminal_panel.ClearTerminalSelection();
        terminal_tab->session.SendMouseMotion(button, viewport_position->row,
                                              viewport_position->column,
                                              render::ToKeyModifiers(interaction_state_.key_modifiers));
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (auto* terminal_tab = ActivePanelTerminalPane()) {
    if (terminal_tab->mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0 &&
        operations_.bottom_panel_visible()) {
      const auto layout_state = operations_.current_workspace_layout();
      if (!layout_state.has_value()) {
        return false;
      }
      const WorkspaceLayout layout = *layout_state;
      const std::size_t line_count = terminal_tab->session.LineCount();
      const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);

      const selection_autoscroll::Band band{
          .rect = panel_layout.content_rect,
          .first_line_y = panel_layout.text_y,
          .line_height = panel_layout.line_height,
          .visible_rows = static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)),
      };
      selection_autoscroll::Arm(interaction_state_, InteractionState::SelectionAutoscrollOwner::Terminal, band,
                                event.motion.x, event.motion.y);
      interaction_state_.selection_pointer_x = event.motion.x;
      interaction_state_.selection_pointer_y = event.motion.y;
      const SDL_FPoint pointer =
          selection_autoscroll::ClampPointerToBand(band, event.motion.x, event.motion.y);

      const std::size_t first_row =
          static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
      thread_local std::vector<terminal::TerminalLine> motion_lines;
      terminal_tab->session.SnapshotLineRangeInto(
          first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)),
          motion_lines);
      const std::vector<terminal::TerminalLine>& terminal_lines = motion_lines;
      if (const auto position = operations_.terminal_selection_position_for_point(
              static_cast<int>(std::lround(pointer.x)),
              static_cast<int>(std::lround(pointer.y)), terminal_lines, first_row);
          position.has_value() && position->row >= first_row &&
          position->row - first_row < terminal_lines.size()) {
        const TerminalSelectionEnds ends =
            ExtendTerminalSelection(terminal_tab->selection_unit, terminal_tab->selection_origin,
                                    terminal_lines[position->row - first_row], *position);
        terminal_tab->selection_anchor = ends.anchor;
        terminal_tab->selection_head = ends.head;
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (TerminalTabState* tab = state_.active_terminal_tab();
      tab != nullptr && ActivePanelTerminalPane() != nullptr) {
    std::optional<TerminalLinkHit> link;
    if (operations_.bottom_panel_visible()) {
      if (const auto layout = operations_.current_workspace_layout();
          layout.has_value() &&
          Contains(operations_.bottom_panel_content_rect(*layout), event.motion.x, event.motion.y)) {
        link = services_.terminal_panel.TerminalLinkAtPoint(event.motion.x, event.motion.y);
      }
    }
    bool changed = false;
    for (std::size_t i = 0; i < tab->pane_count(); ++i) {
      std::optional<TerminalLinkSpan> hovered;
      if (link.has_value() && link->pane == i) {
        hovered = link->span;
      }
      if (hovered != tab->panes[i]->hovered_link) {
        tab->panes[i]->hovered_link = hovered;
        changed = true;
      }
    }
    if (changed) {
      return true;
    }
  }

  return false;
}

bool PanelMouseCoordinator::HandleWheel(const SDL_Event& event,
                                        const WorkspaceLayout& layout,
                                        int vertical_ticks) {
  if (auto* terminal_tab = ActivePanelTerminalPane()) {
    if (terminal_tab->session.WantsMouseCapture()) {
      if (const auto viewport_position =
              operations_.terminal_viewport_position_for_point(event.wheel.mouse_x, event.wheel.mouse_y);
          viewport_position.has_value()) {
        const auto button = vertical_ticks > 0 ? terminal::TerminalSession::MouseButton::WheelUp
                                               : terminal::TerminalSession::MouseButton::WheelDown;
        const int step_count = std::abs(vertical_ticks);
        services_.terminal_panel.ClearTerminalSelection();
        for (int i = 0; i < step_count; ++i) {
          terminal_tab->session.SendMouseButton(button, true, viewport_position->row,
                                                viewport_position->column,
                                                render::ToKeyModifiers(interaction_state_.key_modifiers));
        }
        return true;
      }
    }
  }

  if (!operations_.bottom_panel_visible() ||
      !Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  TerminalPaneState* pane = services_.terminal_panel.PaneAt(
      layout, static_cast<float>(event.wheel.mouse_x), static_cast<float>(event.wheel.mouse_y));
  if (pane != nullptr && pane != state_.active_terminal_pane()) {
    const std::size_t line_count = pane->session.LineCount();
    const int visible_rows =
        operations_.compute_bottom_panel_log_layout(layout, line_count).scroll.visible_rows;
    const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
    const int current = pane->follow_tail
                            ? max_scroll
                            : ClampScrollRowToContent(pane->scroll_row, line_count, visible_rows);
    const int target = ClampScrollRowToContent(current - vertical_ticks * kWheelScrollRows,
                                               line_count, visible_rows);
    pane->scroll_row = target;
    pane->follow_tail = target >= max_scroll;
    return true;
  }

  ScrollPanelRows(layout, -vertical_ticks * kWheelScrollRows);
  return true;
}

void PanelMouseCoordinator::ScrollPanelRows(const WorkspaceLayout& layout, int row_delta) {
  if (state_.panel.content == PanelContentKind::PluginSurface) {
    const editor::SurfaceContent* content = operations_.active_plugin_surface();
    if (content == nullptr || !content->has_body()) {
      return;
    }
    const SDL_FRect body = operations_.bottom_panel_content_rect(layout);
    state_.panel.surface_scroll_y = std::clamp(
        state_.panel.surface_scroll_y +
            static_cast<int>(std::lround(static_cast<float>(row_delta) *
                                         PanelPixelsPerScrollRow(layout))),
        0, MaxPluginSurfacePreviewScroll(*content, body.h));
    return;
  }

  const std::size_t line_count = operations_.bottom_panel_line_count();
  const auto panel_layout = operations_.compute_bottom_panel_log_layout(layout, line_count);
  operations_.set_bottom_panel_scroll_row(
      std::clamp(panel_layout.scroll.vertical_scroll + row_delta, 0,
                 panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
}

int PanelMouseCoordinator::ScrollSpanRows(const WorkspaceLayout& layout) {
  if (state_.panel.content == PanelContentKind::PluginSurface) {
    const editor::SurfaceContent* content = operations_.active_plugin_surface();
    if (content == nullptr || !content->has_body()) {
      return 0;
    }
    const SDL_FRect body = operations_.bottom_panel_content_rect(layout);
    const float step = PanelPixelsPerScrollRow(layout);
    return static_cast<int>(std::lround(
        static_cast<float>(MaxPluginSurfacePreviewScroll(*content, body.h)) / step)) + 1;
  }
  const std::size_t line_count = operations_.bottom_panel_line_count();
  return static_cast<int>(std::min<std::size_t>(
      line_count, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

float PanelMouseCoordinator::PanelPixelsPerScrollRow(const WorkspaceLayout& layout) {
  const float line_height = operations_.compute_bottom_panel_log_layout(layout, 0).line_height;
  return line_height > 0.0f ? line_height : 14.0f;
}

bool PanelMouseCoordinator::ProgramOwnsPointer(const TerminalPaneState& pane) const {
  return pane.session.WantsMouseCapture() && !pane.mouse_selecting &&
         (interaction_state_.key_modifiers & SDL_KMOD_SHIFT) == 0;
}

TerminalPaneState* PanelMouseCoordinator::ActivePanelTerminalPane() {
  return state_.panel.content == PanelContentKind::Terminal ? state_.active_terminal_pane()
                                                            : nullptr;
}

bool PanelMouseCoordinator::HandleMouseCaptureButton(const SDL_Event& event, bool pressed) {
  auto* terminal_tab = ActivePanelTerminalPane();
  if (terminal_tab == nullptr) {
    return false;
  }

  const auto viewport_position =
      operations_.terminal_viewport_position_for_point(event.button.x, event.button.y);
  const auto mouse_button = operations_.terminal_mouse_button_for_sdl(event.button.button);
  if (!viewport_position.has_value() ||
      mouse_button == terminal::TerminalSession::MouseButton::None ||
      !ProgramOwnsPointer(*terminal_tab)) {
    return false;
  }

  services_.terminal_panel.ClearTerminalSelection();
  terminal_tab->session.SendMouseButton(mouse_button, pressed, viewport_position->row,
                                        viewport_position->column,
                                        render::ToKeyModifiers(interaction_state_.key_modifiers));
  state_.surface.focus = FocusTarget::Panel;
  return true;
}

PanelMouseCoordinator& WorkspaceShell::MakePanelMouseCoordinator() {
  if (glue_->panel_mouse_coordinator != nullptr) {
    return *glue_->panel_mouse_coordinator;
  }
  glue_->panel_mouse_coordinator = std::make_unique<PanelMouseCoordinator>(
      context_.current_project_state, context_.interaction_state,
      PanelMouseCoordinator::Services{
          .terminal_panel = MakeTerminalPanelService(),
          .menus = MakeMenuCoordinator(),
      },
      PanelMouseCoordinator::Operations{
          .bottom_panel_visible = [this]() { return BottomPanelVisible(); },
          .bottom_panel_resize_handle_rect =
              [](const WorkspaceLayout& layout) { return BottomPanelResizeHitRect(layout); },
          .compute_bottom_panel_log_layout =
              [this](const WorkspaceLayout& layout, std::size_t line_count) {
                return ComputeBottomPanelLogLayout(layout, line_count);
              },
          .bottom_panel_line_count =
              [this]() {
                if (BottomPanelShowsTerminal() && ActiveTerminalPane() != nullptr) {
                  return ActiveTerminalPane()->session.LineCount();
                }
                if (BottomPanelShowsOutput()) {
                  if (const auto* entries =
                          OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
                      entries != nullptr) {
                    return entries->size();
                  }
                  return std::size_t{0};
                }
                return std::size_t{0};
              },
          .set_bottom_panel_scroll_row = [this](
                                             int row, std::size_t count,
                                             int visible_rows) { SetBottomPanelScrollRow(row, count, visible_rows); },
          .bottom_panel_content_rect = [](const WorkspaceLayout& layout) { return BottomPanelContentRect(layout); },
          .terminal_find_mouse_down = [this](float x, float y) { return HandleTerminalFindMouseDown(x, y); },
          .open_external_url =
              [this](std::string_view url) {
                return MakeTerminalPanelService().OpenUrlForPane(
                    context_.current_project_state.active_terminal_pane(), url);
              },
          .terminal_selection_position_for_point =
              [this](int x, int y, const std::vector<terminal::TerminalLine>& lines, std::size_t first_row) {
                return TerminalSelectionPointAt(x, y, lines, first_row);
              },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .terminal_viewport_position_for_point = [this](int x,
                                                         int y) { return TerminalViewportPositionForPoint(x, y); },
          .terminal_mouse_button_for_sdl = [](Uint8 button) { return TerminalMouseButtonFromSdl(button); },
          .output_channel_entries =
              [this]() { return OutputChannelEntries(context_.current_project_state.panel.output.channel_id); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .clamp_bottom_panel_height =
              [](float desired_height, float window_height) {
                return ClampBottomPanelHeight(desired_height, window_height);
              },
          .active_plugin_surface = [this]() -> const editor::SurfaceContent* {
            const auto& panel = context_.current_project_state.panel;
            const auto* pres = context_.current_project_state.plugin_presentation_if_present();
            return pres != nullptr ? pres->surfaces.Find(panel.surface_owner, panel.surface_id) : nullptr;
          },
          .execute_command =
              [this](const std::string& command) {
                std::string error_message;
                ExecuteCommandName(command, {}, ActionSource::Command, &error_message);
              },
      });
  return *glue_->panel_mouse_coordinator;
}

}
