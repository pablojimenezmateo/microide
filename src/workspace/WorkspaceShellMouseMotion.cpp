#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspaceMergeMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  bool hover_visual_changed = false;
  if (CurrentWindowRect().has_value()) {
    const std::optional<EditorHoverTarget> previous_hover_target = active_editor_hover_target_;
    const bool previous_action_hovered =
        last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    const bool current_action_hovered =
        EditorHoverPopupPrimaryActionHovered(static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y));
    hover_visual_changed = !(previous_hover_target == active_editor_hover_target_) ||
                           previous_action_hovered != current_action_hovered;
  }

  if (prompts_.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (prompts_.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;
  if (ChromeMouseCoordinator(*this).HandleMotion(event, layout)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (tab_drag_state_.kind != TabDragKind::None) {
    const bool handled = TabMouseCoordinator(*this).HandleMotion(event);
    if (handled) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
    }
    return handled;
  }

  if (surface_.drag_target != DragTarget::None) {
    if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
      surface_.drag_target = DragTarget::None;
      surface_.drag_scrollbar_offset = 0.0f;
      surface_.drag_editor_split_path.clear();
      surface_.drag_editor_split_divider_index = 0;
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      return false;
    }

    const WorkspaceLayout drag_layout = layout;

    if (surface_.drag_target == DragTarget::SidebarDivider) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      surface_.sidebar_width = ClampSidebarWidth(static_cast<float>(event.motion.x), window_width);
      RequestSidebarLayoutChangeRedraw(drag_layout);
      return true;
    }

    if (surface_.drag_target == DragTarget::BottomPanelDivider) {
      if (PanelMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
        RequestBottomPanelLayoutChangeRedraw(drag_layout);
        return true;
      }
    }

    if (PanelMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestBottomPanelRedraw(); });
      return true;
    }

    if (MergeMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (surface_.drag_target == DragTarget::SidebarScrollbar) {
      const bool handled = SidebarMouseCoordinator(*this).HandleDrag(event, drag_layout);
      if (handled) {
        ensure_redraw([this]() { RequestSidebarRedraw(); });
      }
      return handled;
    }

    if (surface_.drag_target == DragTarget::OverlayScrollbar && surface_.overlay_visible) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.editor_area);
      const auto list_layout = ComputeOverlayListLayout(overlay);
      if (!list_layout.scrollbar.has_value()) {
        surface_.drag_target = DragTarget::None;
        surface_.drag_scrollbar_offset = 0.0f;
        return false;
      }
      surface_.overlay_scroll_row =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*list_layout.scrollbar,
                                              static_cast<float>(event.motion.y),
                                              surface_.drag_scrollbar_offset))),
                     0, list_layout.max_scroll);
      surface_.focus = FocusTarget::Overlay;
      ensure_redraw([this]() { RequestOverlayRedraw(); });
      return true;
    }

    if (CompareMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (EditorMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    surface_.drag_target = DragTarget::None;
    surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  if (PanelMouseCoordinator(*this).HandleMotion(event)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (ActiveTabIsMerge()) {
    hover_visual_changed =
        hover_visual_changed ||
        MergeMouseCoordinator(*this).HandleHoverMotion(event, layout);
  }

  if (hover_visual_changed) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }

  if (!surface_.mouse_selecting || (event.motion.state & SDL_BUTTON_LMASK) == 0) {
    return hover_visual_changed;
  }

  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    const bool handled = CompareMouseCoordinator(*this).HandleSelectionMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge()) {
    const bool handled = MergeMouseCoordinator(*this).HandleSelectionMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  const bool handled = EditorMouseCoordinator(*this).HandleSelectionMotion(event, layout);
  if (handled) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return handled;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  if (prompts_.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (prompts_.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (menu_state_.menu_bar_open || menu_state_.tree_context_menu.open) {
    ensure_redraw([this]() { RequestChromeRedraw(); });
    return true;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }

  const int vertical_ticks = event.wheel.integer_y != 0
                                 ? event.wheel.integer_y
                                 : static_cast<int>(std::lround(event.wheel.y));
  const int axis_horizontal_ticks = event.wheel.integer_x != 0
                                        ? event.wheel.integer_x
                                        : static_cast<int>(std::lround(event.wheel.x));
  const int horizontal_ticks =
      axis_horizontal_ticks != 0
          ? axis_horizontal_ticks
          : ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0 ? -vertical_ticks : 0);
  if (vertical_ticks == 0 && horizontal_ticks == 0) {
    return false;
  }

  const WorkspaceLayout layout = *layout_state;

  if (ChromeMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks, horizontal_ticks)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (TabMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (SidebarMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      const bool handled =
          CompareMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    if (ActiveTabIsMerge()) {
      const bool handled =
          MergeMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    const bool handled = EditorMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  return false;
}

}  // namespace microide::workspace
