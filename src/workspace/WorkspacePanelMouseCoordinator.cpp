#include "workspace/WorkspacePanelMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::PanelMouseCoordinator::PanelMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::PanelMouseCoordinator::HandleResizeButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !shell_.BottomPanelVisible() ||
      !Contains(BottomPanelResizeHandleRect(layout), event.button.x, event.button.y)) {
    return false;
  }
  shell_.surface_.drag_target = DragTarget::BottomPanelDivider;
  return true;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                                             const WorkspaceLayout& layout) {
  if (event.button.button == SDL_BUTTON_LEFT && shell_.BottomPanelVisible()) {
    const std::size_t line_count =
        shell_.ActiveTerminalTab() != nullptr
            ? shell_.ActiveTerminalTab()->session.LineCount()
            : 0;
    const BottomPanelLogLayout panel_layout =
        shell_.ComputeBottomPanelLogLayout(layout, line_count);
    if (panel_layout.scroll.vertical_scrollbar.has_value() &&
        Contains(panel_layout.scroll.vertical_scrollbar->track, event.button.x,
                 event.button.y)) {
      shell_.surface_.drag_target = DragTarget::BottomPanelScrollbar;
      shell_.surface_.drag_scrollbar_offset =
          Contains(panel_layout.scroll.vertical_scrollbar->thumb, event.button.x,
                   event.button.y)
              ? static_cast<float>(event.button.y) -
                    panel_layout.scroll.vertical_scrollbar->thumb.y
              : panel_layout.scroll.vertical_scrollbar->thumb.h * 0.5f;
      shell_.SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*panel_layout.scroll.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              shell_.surface_.drag_scrollbar_offset))),
                     0, panel_layout.scroll.max_vertical_scroll),
          line_count, panel_layout.scroll.visible_rows);
      if (shell_.ActiveTerminalTab() != nullptr) {
        shell_.surface_.focus = FocusTarget::Panel;
      }
      return true;
    }
  }

  if (HandleMouseCaptureButton(event, true)) {
    return true;
  }

  if (!shell_.BottomPanelVisible() ||
      !Contains(layout.bottom_panel, event.button.x, event.button.y)) {
    return false;
  }

  if (shell_.ActiveTerminalTab() != nullptr) {
    const SDL_FRect panel_content =
        BottomPanelContentRect(layout, shell_.surface_.command_mode);
    if (event.button.button == SDL_BUTTON_RIGHT && Contains(panel_content, event.button.x,
                                                            event.button.y)) {
      shell_.surface_.focus = FocusTarget::Panel;
      MenuCoordinator(shell_).OpenAnchoredMenu(
          MenuId::TerminalContext,
          MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                   1.0f));
      return true;
    }
    if (event.button.button == SDL_BUTTON_MIDDLE && Contains(panel_content, event.button.x,
                                                             event.button.y)) {
      if (const std::optional<std::string> text = shell_.ReadPrimarySelectionText();
          text.has_value()) {
        shell_.ClearTerminalSelection();
        if (auto* terminal_tab = shell_.ActiveTerminalTab(); terminal_tab != nullptr) {
          terminal_tab->follow_tail = true;
          shell_.AppendTerminalPendingInput(*text);
          terminal_tab->session.PasteText(*text);
        }
      }
      shell_.surface_.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  if (shell_.ActiveTerminalTab() != nullptr) {
    const SDL_FRect panel_content =
        BottomPanelContentRect(layout, shell_.surface_.command_mode);
    if (Contains(panel_content, event.button.x, event.button.y)) {
      if (const std::optional<std::string> url =
              shell_.TerminalUrlAtPoint(static_cast<float>(event.button.x),
                                        static_cast<float>(event.button.y));
          url.has_value() && shell_.OpenExternalUrl(*url)) {
        shell_.surface_.focus = FocusTarget::Panel;
        return true;
      }
      const std::size_t line_count = shell_.ActiveTerminalTab()->session.LineCount();
      const BottomPanelLogLayout panel_layout =
          shell_.ComputeBottomPanelLogLayout(layout, line_count);
      const std::size_t first_row =
          static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
      const auto terminal_lines = shell_.ActiveTerminalTab()->session.SnapshotLineRange(
          first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)));
      if (const auto position = shell_.TerminalSelectionPositionForPoint(
              event.button.x, event.button.y, terminal_lines, first_row);
          position.has_value()) {
        if (auto* terminal_tab = shell_.ActiveTerminalTab(); terminal_tab != nullptr) {
          terminal_tab->selection_anchor = *position;
          terminal_tab->selection_head = *position;
          terminal_tab->mouse_selecting = true;
          terminal_tab->follow_tail = false;
        }
      } else {
        shell_.ClearTerminalSelection();
      }
    } else {
      shell_.ClearTerminalSelection();
    }
    shell_.surface_.focus = FocusTarget::Panel;
  }

  if (shell_.surface_.command_mode) {
    shell_.surface_.focus = FocusTarget::Panel;
  }
  return true;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleButtonUp(const SDL_Event& event) {
  if (HandleMouseCaptureButton(event, false)) {
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  if (shell_.surface_.drag_target == DragTarget::BottomPanelDivider ||
      shell_.surface_.drag_target == DragTarget::BottomPanelScrollbar) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    shell_.surface_.mouse_selecting = false;
    return true;
  }

  if (auto* terminal_tab = shell_.ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting) {
    terminal_tab->mouse_selecting = false;
    shell_.SyncPrimarySelectionWithTerminalSelection();
    return true;
  }

  return false;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleDrag(const SDL_Event& event,
                                                       const WorkspaceLayout& layout) {
  if (shell_.surface_.drag_target == DragTarget::BottomPanelDivider) {
    const auto window_rect = shell_.CurrentWindowRect();
    if (!window_rect.has_value()) {
      return false;
    }
    const float desired_height = window_rect->h - static_cast<float>(event.motion.y);
    shell_.surface_.bottom_panel_height =
        ClampBottomPanelHeight(desired_height, window_rect->h);
    return true;
  }

  if (shell_.surface_.drag_target != DragTarget::BottomPanelScrollbar ||
      !shell_.BottomPanelVisible()) {
    return false;
  }

  const std::size_t line_count =
      shell_.ActiveTerminalTab() != nullptr
          ? shell_.ActiveTerminalTab()->session.LineCount()
          : 0;
  const BottomPanelLogLayout panel_layout =
      shell_.ComputeBottomPanelLogLayout(layout, line_count);
  if (!panel_layout.scroll.vertical_scrollbar.has_value()) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }
  shell_.SetBottomPanelScrollRow(
      std::clamp(static_cast<int>(std::lround(ScrollUnitsForPointer(
                     *panel_layout.scroll.vertical_scrollbar,
                     static_cast<float>(event.motion.y),
                     shell_.surface_.drag_scrollbar_offset))),
                 0, panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  if (shell_.ActiveTerminalTab() != nullptr) {
    shell_.surface_.focus = FocusTarget::Panel;
  }
  return true;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (shell_.ActiveTerminalTab() != nullptr) {
    if (auto* terminal_tab = shell_.ActiveTerminalTab(); terminal_tab != nullptr) {
      const bool buttons_down =
          (event.motion.state &
           (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
      if (terminal_tab->session.WantsMouseMotionCapture(buttons_down)) {
        if (const auto viewport_position =
                shell_.TerminalViewportPositionForPoint(event.motion.x,
                                                       event.motion.y);
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
          shell_.ClearTerminalSelection();
          terminal_tab->session.SendMouseMotion(button, viewport_position->row,
                                                viewport_position->column,
                                                SDL_GetModState());
          shell_.surface_.focus = FocusTarget::Panel;
          return true;
        }
      }
    }
  }

  if (auto* terminal_tab = shell_.ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting &&
      (event.motion.state & SDL_BUTTON_LMASK) != 0 && shell_.BottomPanelVisible() &&
      shell_.ActiveTerminalTab() != nullptr) {
    const auto layout_state = shell_.CurrentWorkspaceLayout();
    if (!layout_state.has_value()) {
      return false;
    }
    const WorkspaceLayout layout = *layout_state;
    if (!Contains(layout.bottom_panel, event.motion.x, event.motion.y)) {
      return false;
    }

    const std::size_t line_count = terminal_tab->session.LineCount();
    const BottomPanelLogLayout panel_layout =
        shell_.ComputeBottomPanelLogLayout(layout, line_count);
    const std::size_t first_row =
        static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
    const auto terminal_lines = terminal_tab->session.SnapshotLineRange(
        first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)));
    if (const auto position = shell_.TerminalSelectionPositionForPoint(
            event.motion.x, event.motion.y, terminal_lines, first_row);
        position.has_value()) {
      terminal_tab->selection_head = *position;
      shell_.surface_.focus = FocusTarget::Panel;
      return true;
    }
  }

  return false;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                        const WorkspaceLayout& layout,
                                                        int vertical_ticks) {
  if (shell_.ActiveTerminalTab() != nullptr) {
    if (auto* terminal_tab = shell_.ActiveTerminalTab();
        terminal_tab != nullptr && terminal_tab->session.WantsMouseCapture()) {
      if (const auto viewport_position = shell_.TerminalViewportPositionForPoint(
              event.wheel.mouse_x, event.wheel.mouse_y);
          viewport_position.has_value()) {
        const terminal::TerminalSession::MouseButton button =
            vertical_ticks > 0 ? terminal::TerminalSession::MouseButton::WheelUp
                               : terminal::TerminalSession::MouseButton::WheelDown;
        const int step_count = std::abs(vertical_ticks);
        shell_.ClearTerminalSelection();
        for (int i = 0; i < step_count; ++i) {
          terminal_tab->session.SendMouseButton(button, true, viewport_position->row,
                                                viewport_position->column,
                                                SDL_GetModState());
        }
        shell_.surface_.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (!shell_.BottomPanelVisible() ||
      !Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const std::size_t line_count =
      shell_.ActiveTerminalTab() != nullptr
          ? shell_.ActiveTerminalTab()->session.LineCount()
          : 0;
  const BottomPanelLogLayout panel_layout =
      shell_.ComputeBottomPanelLogLayout(layout, line_count);
  shell_.SetBottomPanelScrollRow(
      std::clamp(panel_layout.scroll.vertical_scroll - vertical_ticks, 0,
                 panel_layout.scroll.max_vertical_scroll),
      line_count, panel_layout.scroll.visible_rows);
  if (shell_.ActiveTerminalTab() != nullptr) {
    shell_.surface_.focus = FocusTarget::Panel;
  }
  return true;
}

bool WorkspaceShell::PanelMouseCoordinator::HandleMouseCaptureButton(
    const SDL_Event& event,
    bool pressed) {
  if (shell_.ActiveTerminalTab() == nullptr) {
    return false;
  }

  auto* terminal_tab = shell_.ActiveTerminalTab();
  const auto viewport_position =
      shell_.TerminalViewportPositionForPoint(event.button.x, event.button.y);
  const auto mouse_button = shell_.TerminalMouseButtonForSdl(event.button.button);
  if (terminal_tab == nullptr || !viewport_position.has_value() ||
      mouse_button == terminal::TerminalSession::MouseButton::None ||
      !terminal_tab->session.WantsMouseCapture()) {
    return false;
  }

  shell_.ClearTerminalSelection();
  terminal_tab->session.SendMouseButton(mouse_button, pressed, viewport_position->row,
                                        viewport_position->column, SDL_GetModState());
  shell_.surface_.focus = FocusTarget::Panel;
  return true;
}

}  // namespace microide::workspace
