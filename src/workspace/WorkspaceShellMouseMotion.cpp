#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspaceMergeMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseMotion");
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  bool hover_visual_changed = false;
  std::optional<SDL_FRect> previous_project_tab_tooltip_rect;
  std::optional<SDL_FRect> previous_tab_tooltip_rect;
  std::optional<SDL_FRect> previous_status_tooltip_rect;
  std::string previous_project_tab_tooltip_label;
  std::string previous_tab_tooltip_label;
  std::string previous_status_tooltip_label;
  if (CurrentWindowRect().has_value()) {
    const auto layout_before_state = CurrentWorkspaceLayout();
    if (layout_before_state.has_value()) {
      previous_project_tab_tooltip_label =
          HoveredProjectTabTooltipLabel(layout_before_state->project_tab_strip);
      previous_project_tab_tooltip_rect =
          HoveredProjectTabTooltipRect(*layout_before_state);
      previous_tab_tooltip_label = HoveredTabTooltipLabel(layout_before_state->tab_strip);
      previous_tab_tooltip_rect = HoveredTabTooltipRect(*layout_before_state);
      previous_status_tooltip_label = HoveredStatusTooltip(layout_before_state->breadcrumb);
      previous_status_tooltip_rect = HoveredStatusTooltipRect(*layout_before_state);
    }
    const std::optional<EditorHoverTarget> previous_hover_target = active_editor_hover_target_;
    const bool previous_action_hovered =
        last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::HandleMouseMotion::UpdateMouseCursor");
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    }
    const bool current_action_hovered =
        EditorHoverPopupPrimaryActionHovered(static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y));
    hover_visual_changed = !(previous_hover_target == active_editor_hover_target_) ||
                           previous_action_hovered != current_action_hovered;
  }

  if (context_.prompts.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;
  const auto rects_equal = [](const SDL_FRect& lhs, const SDL_FRect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
  };
  const auto request_tooltip_redraw_if_changed =
      [this, &rects_equal](const std::string& previous_label,
                           const std::optional<SDL_FRect>& previous_rect,
                           const std::string& current_label,
                           const std::optional<SDL_FRect>& current_rect) {
        const auto padded_rect = [](const SDL_FRect& rect) {
          static constexpr float kTooltipRedrawPaddingPx = 1.0f;
          return MakeRect(rect.x - kTooltipRedrawPaddingPx, rect.y - kTooltipRedrawPaddingPx,
                          rect.w + 2.0f * kTooltipRedrawPaddingPx,
                          rect.h + 2.0f * kTooltipRedrawPaddingPx);
        };
        const bool same_rect = previous_rect.has_value() == current_rect.has_value() &&
                               (!previous_rect.has_value() ||
                                rects_equal(*previous_rect, *current_rect));
        if (previous_label == current_label && same_rect) {
          return false;
        }
        if (previous_rect.has_value()) {
          RequestRedrawRect(padded_rect(*previous_rect));
        }
        if (current_rect.has_value()) {
          RequestRedrawRect(padded_rect(*current_rect));
        }
        return true;
      };
  const bool chrome_tooltip_visual_changed =
      request_tooltip_redraw_if_changed(previous_project_tab_tooltip_label,
                                        previous_project_tab_tooltip_rect,
                                        HoveredProjectTabTooltipLabel(layout.project_tab_strip),
                                        HoveredProjectTabTooltipRect(layout)) ||
      request_tooltip_redraw_if_changed(previous_tab_tooltip_label, previous_tab_tooltip_rect,
                                        HoveredTabTooltipLabel(layout.tab_strip),
                                        HoveredTabTooltipRect(layout)) ||
      request_tooltip_redraw_if_changed(previous_status_tooltip_label,
                                        previous_status_tooltip_rect,
                                        HoveredStatusTooltip(layout.breadcrumb),
                                        HoveredStatusTooltipRect(layout));
  if (MakeChromeMouseCoordinator().HandleMotion(event, layout)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    const bool handled = HandleTabMouseMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
    }
    return handled;
  }

  if (context_.interaction_state.drag_target != DragTarget::None) {
    if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
      ClearDragState();
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      return false;
    }

    const WorkspaceLayout drag_layout = layout;

    if (context_.interaction_state.drag_target == DragTarget::SidebarDivider) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      context_.current_project_state.sidebar.width = ClampSidebarWidth(static_cast<float>(event.motion.x), window_width);
      RequestSidebarLayoutChangeRedraw(drag_layout);
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::BottomPanelDivider) {
      if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
        RequestBottomPanelLayoutChangeRedraw(drag_layout);
        return true;
      }
    }

    if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestBottomPanelRedraw(); });
      return true;
    }

    if (MakeMergeMouseCoordinator().HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::SidebarScrollbar) {
      const bool handled = MakeSidebarMouseCoordinator().HandleDrag(event, drag_layout);
      if (handled) {
        ensure_redraw([this]() { RequestSidebarRedraw(); });
      }
      return handled;
    }

    if (context_.interaction_state.drag_target == DragTarget::OverlayScrollbar && context_.current_project_state.overlay.visible) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.editor_area);
      const auto list_layout = ComputeOverlayListLayout(overlay);
      if (!list_layout.scrollbar.has_value()) {
        ClearDragState();
        return false;
      }
      context_.current_project_state.overlay.scroll_row =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*list_layout.scrollbar,
                                              static_cast<float>(event.motion.y),
                                              context_.interaction_state.drag_scrollbar_offset))),
                     0, list_layout.max_scroll);
      context_.current_project_state.surface.focus = FocusTarget::Overlay;
      ensure_redraw([this]() { RequestOverlayRedraw(); });
      return true;
    }

    if (MakeCompareMouseCoordinator().HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (MakeEditorMouseCoordinator().HandleDrag(event, drag_layout)) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    ClearDragState();
    return false;
  }

  if (MakePanelMouseCoordinator().HandleMotion(event)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (ActiveTabIsMerge()) {
    hover_visual_changed =
        hover_visual_changed ||
        MakeMergeMouseCoordinator().HandleHoverMotion(event, layout);
  }

  if (hover_visual_changed) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }

  if (!context_.interaction_state.mouse_selecting || (event.motion.state & SDL_BUTTON_LMASK) == 0) {
    return hover_visual_changed || chrome_tooltip_visual_changed;
  }

  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    const bool handled = MakeCompareMouseCoordinator().HandleSelectionMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge()) {
    const bool handled = MakeMergeMouseCoordinator().HandleSelectionMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  const bool handled = MakeEditorMouseCoordinator().HandleSelectionMotion(event, layout);
  if (handled) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return handled;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseWheel");
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  if (context_.prompts.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (context_.menu_state.menu_bar_open || context_.menu_state.tree_context_menu.open) {
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

  if (MakeChromeMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (HandleTabMouseWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      const bool handled =
          MakeCompareMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    if (ActiveTabIsMerge()) {
      const bool handled =
          MakeMergeMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    const bool handled = MakeEditorMouseCoordinator().HandleWheel(event, layout, vertical_ticks);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  return false;
}

}  // namespace microide::workspace
