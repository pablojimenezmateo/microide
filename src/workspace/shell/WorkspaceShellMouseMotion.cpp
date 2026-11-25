#include "workspace/render/NotificationToastPaint.h"
#include "workspace/shell/WorkspaceShell.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/git/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

#include "util/PerformanceTrace.h"
#include "workspace/ListSelection.h"
#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

namespace {

bool HoverChanged(const std::optional<SDL_FRect>& previous, const std::optional<SDL_FRect>& current) {
  if (previous.has_value() != current.has_value()) {
    return true;
  }
  return previous.has_value() && current.has_value() && !RectsEqual(*previous, *current);
}

}

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseMotion");
  const DamageMark mark = MarkDamage();
  if (context_.prompts.dirty_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;

  if (context_.interaction_state.drag_target == DragTarget::None &&
      !notification_service_.Empty()) {
    const float x = static_cast<float>(event.motion.x);
    const float y = static_cast<float>(event.motion.y);
    const NotificationsViewModel toasts =
        RenderViewModelBuilder(context_).BuildNotifications(notification_service_, layout.status_bar, text_renderer_);
    const std::optional<NotificationHit> hit = NotificationHitAt(toasts, x, y);
    const bool changed = notification_service_.SetHovered(
        hit.has_value() ? std::optional<std::size_t>(hit->index) : std::nullopt,
        hit.has_value() ? hit->action : std::nullopt, SDL_GetTicks());
    if (changed) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    if (hit.has_value()) {
      UpdateMouseCursor(x, y, false);
      return true;
    }
  }

  if (context_.interaction_state.drag_target == DragTarget::SingleLineSelection) {
    if (HandleSingleLineInputDrag(event, layout)) {
      RequestSingleLineDragSurfaceRedraw(mark, context_.interaction_state.single_line_drag_surface);
      return true;
    }
  }

  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsCategoryScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsPickerScrollbar) {
    const DragTarget target = context_.interaction_state.drag_target;
    if (settings_overlay_service_.Visible()) {
      const SettingsOverlayViewModel vm =
          RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                                text_renderer_);
      const std::optional<ScrollbarGeometry>* bar = &vm.scrollbar;
      int max_scroll = vm.max_scroll;
      if (target == DragTarget::SettingsCategoryScrollbar) {
        bar = &vm.category_scrollbar;
        max_scroll = vm.category_max_scroll;
      } else if (target == DragTarget::SettingsPickerScrollbar) {
        bar = &vm.value_picker.scrollbar;
        max_scroll = vm.value_picker.max_scroll;
      }
      if (bar->has_value()) {
        const int row = std::clamp(
            static_cast<int>(std::lround(ScrollUnitsForPointer(
                **bar, static_cast<float>(event.motion.y),
                context_.interaction_state.drag_scrollbar_offset))),
            0, max_scroll);
        switch (target) {
          case DragTarget::SettingsCategoryScrollbar:
            settings_overlay_service_.SetCategoryScrollRow(row);
            break;
          case DragTarget::SettingsPickerScrollbar:
            settings_overlay_service_.SetPickerScroll(row);
            break;
          default:
            settings_overlay_service_.SetScrollRow(row);
            break;
        }
      }
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (context_.prompts.surface_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (settings_overlay_service_.Visible()) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  const bool previous_mouse_position_valid = last_mouse_position_valid_;
  const float previous_mouse_x = last_mouse_x_;
  const float previous_mouse_y = last_mouse_y_;
  if (const auto decided = MotionDuringDrag(event, layout, mark)) {
    return *decided;
  }
  if (const auto decided = MotionDuringPointerGesture(event, layout, mark)) {
    return *decided;
  }
  if (const auto decided = MotionOverOpenMenus(event, layout)) {
    return *decided;
  }
  return MotionHover(event, layout, mark, previous_mouse_position_valid, previous_mouse_x, previous_mouse_y);
}

std::optional<bool> WorkspaceShell::MotionDuringDrag(const SDL_Event& event, const WorkspaceLayout& layout,
                                                     DamageMark mark) {
  if (context_.interaction_state.drag_target != DragTarget::None) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
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

    if (context_.interaction_state.drag_target == DragTarget::RightPaneDivider) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      const float resolved_sidebar_width = context_.current_project_state.sidebar.visible
                                               ? drag_layout.sidebar.w
                                               : 0.0f;
      context_.current_project_state.debug_pane.width = ClampRightPaneWidth(
          window_width - static_cast<float>(event.motion.x), window_width, resolved_sidebar_width);
      NoteLayoutInputsChanged();
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::EditorSplitDivider) {
      const std::uint8_t node = context_.interaction_state.drag_editor_split_node;
      const std::uint8_t boundary = context_.interaction_state.drag_editor_split_boundary;
      for (const EditorSplitDividerRect& divider :
           ComputeEditorGroupRectsForState(drag_layout).dividers) {
        if (divider.node != node || divider.boundary != boundary || divider.pair_extent <= 0.0f) {
          continue;
        }
        const float pointer = divider.vertical ? static_cast<float>(event.motion.x)
                                               : static_cast<float>(event.motion.y);
        context_.current_project_state.editor_split.ResizeDivider(
            node, boundary, (pointer - divider.pair_start) / divider.pair_extent);
        NoteLayoutInputsChanged();
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        break;
      }
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::BottomPanelDivider) {
      if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
        RequestBottomPanelLayoutChangeRedraw(drag_layout);
        return true;
      }
    }

    if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    if (MakeMergeMouseCoordinator().HandleDrag(event, ActiveTabSurface(drag_layout))) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::SidebarScrollbar) {
      const bool handled = MakeSidebarMouseCoordinator().HandleDrag(event, drag_layout);
      if (handled) {
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      }
      return handled;
    }

    if (context_.interaction_state.drag_target == DragTarget::DebugPaneScrollbar) {
      const bool handled = MakeDebugPaneMouseCoordinator().HandleDrag(event, drag_layout);
      if (handled) {
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      }
      return handled;
    }

    if (context_.interaction_state.drag_target == DragTarget::OverlayScrollbar &&
        context_.current_project_state.overlay.visible) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.overlay_anchor);
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
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    if (MakeCompareMouseCoordinator().HandleDrag(event, ActiveTabSurface(drag_layout))) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    if (MakeEditorMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }

    ClearDragState();
    return false;
  }
  return std::nullopt;
}

std::optional<bool> WorkspaceShell::MotionDuringPointerGesture(const SDL_Event& event, const WorkspaceLayout& layout,
                                                               DamageMark mark) {
  if (context_.interaction_state.path_drag.armed || context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
  }
  if (context_.interaction_state.path_drag.armed) {
    if (MakeSidebarMouseCoordinator().HandlePathDragMotion(static_cast<float>(event.motion.x),
                                                           static_cast<float>(event.motion.y), layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return true;
  }

  if (context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    const bool handled = HandleTabMouseMotion(event, layout);
    if (handled) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return handled;
  }

  if (context_.interaction_state.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0) {
    if (ActiveTabIsCompare()) {
      const bool handled = MakeCompareMouseCoordinator().HandleSelectionMotion(event, ActiveTabSurface(layout));
      if (handled) {
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      }
      return handled;
    }

    if (ActiveTabIsMerge()) {
      const bool handled = MakeMergeMouseCoordinator().HandleSelectionMotion(event, ActiveTabSurface(layout));
      if (handled) {
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      }
      return handled;
    }

    const bool handled = MakeEditorMouseCoordinator().HandleSelectionMotion(event, layout);
    if (handled) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return handled;
  }
  return std::nullopt;
}

std::optional<bool> WorkspaceShell::MotionOverOpenMenus(const SDL_Event& event, const WorkspaceLayout& layout) {
  if (MenuSurfaceCapturingMouse()) {
    static const bool menu_hover_trace =
        util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_MENU_HOVER");
    const Uint64 t0 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    const std::size_t rects_before =
        menu_hover_trace ? pending_render_invalidation_.rects.size() : 0;
    {
      const float x = static_cast<float>(event.motion.x);
      const float y = static_cast<float>(event.motion.y);
      bool taken = false;
      for (std::size_t slot = frame_state_.menu_popups.size(); slot-- > 0;) {
        const MenuPopupPaint& popup = frame_state_.menu_popups[slot];
        if (!popup.visible || !popup.follows_pointer) {
          continue;
        }
        int row_under_pointer = -1;
        if (!taken && Contains(popup.rect, x, y)) {
          taken = true;
          for (std::size_t row = 0; row < popup.rows.size(); ++row) {
            if (!popup.rows[row].separator && Contains(popup.rows[row].rect, x, y)) {
              row_under_pointer = static_cast<int>(row);
              break;
            }
          }
        }
        if (row_under_pointer != popup.pointer_row) {
          RequestPreparedFrame();
          break;
        }
      }
    }
    const Uint64 t1 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    const Uint64 t3 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    ChromeMouseCoordinator& chrome_coordinator = MakeChromeMouseCoordinator();
    const Uint64 t4 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    chrome_coordinator.HandleMotion(event, layout);
    const Uint64 t5 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    if (menu_hover_trace) {
      const auto ms = [](Uint64 a, Uint64 b) {
        return static_cast<double>(b - a) / 1'000'000.0;
      };
      SDL_Log("MICROIDE_MENU_HOVER motion ev=(%.0f,%.0f) total=%.3fms "
              "[capture=%.3f cursor=%.3f makeCoord=%.3f handle=%.3f] "
              "rects+=%zu ami=%d hovered=%d",
              event.motion.x, event.motion.y, ms(t0, t5),
              ms(t0, t1), ms(t1, t3), ms(t3, t4), ms(t4, t5),
              pending_render_invalidation_.rects.size() - rects_before,
              context_.menu_state.active_menu_item_index,
              context_.menu_state.hovered_popup_row_index);
    }
    return true;
  }
  return std::nullopt;
}

bool WorkspaceShell::MotionHover(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark,
                                 bool previous_mouse_position_valid, float previous_mouse_x, float previous_mouse_y) {
  bool hover_visual_changed = false;
  bool sidebar_hover_button_changed = false;
  bool status_segment_hover_changed = false;
  std::optional<HoverTooltip> previous_tooltip;
  if (CurrentWindowRect().has_value()) {
    previous_tooltip = HoveredTooltip(layout);
    const std::optional<EditorHoverTarget> previous_hover_target = active_editor_hover_target_;
    const bool previous_action_hovered =
        last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::HandleMouseMotion::UpdateMouseCursor");
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    }
    if (HoverChanged(previous_mouse_position_valid ? HoveredInteractiveRect(layout, previous_mouse_x, previous_mouse_y)
                                                   : std::nullopt,
                     HoveredInteractiveRect(layout, static_cast<float>(event.motion.x),
                                            static_cast<float>(event.motion.y)))) {
      sidebar_hover_button_changed = true;
    }
    {
      const auto hovered_tab_close = [&](float x, float y) -> std::optional<SDL_FRect> {
        const auto close_of = [x, y](std::span<const VisibleStripTab> tabs) -> std::optional<SDL_FRect> {
          for (const VisibleStripTab& tab : tabs) {
            if (tab.close_rect.w > 0.0f && tab.close_rect.h > 0.0f && Contains(tab.close_rect, x, y)) {
              return tab.close_rect;
            }
          }
          return std::nullopt;
        };
        if (layout.project_tab_strip.w > 0.0f && Contains(layout.project_tab_strip, x, y)) {
          return close_of(tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip));
        }
        const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
        for (std::size_t gi = 0; gi < group_rects.groups.size(); ++gi) {
          const SDL_FRect strip = group_rects.groups[gi].tab_strip;
          if (strip.w > 0.0f && strip.h > 0.0f && Contains(strip, x, y)) {
            return close_of(tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip));
          }
        }
        if (BottomPanelVisible()) {
          const SDL_FRect panel_header =
              MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                       kWorkspaceBottomPanelHeaderHeight);
          if (Contains(panel_header, x, y)) {
            return close_of(tab_strip_service_.ComputeVisibleBottomPanelTabs(
                context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
                [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
                output_channels_.Channels()));
          }
        }
        return std::nullopt;
      };
      const std::optional<SDL_FRect> previous_close =
          previous_mouse_position_valid ? hovered_tab_close(previous_mouse_x, previous_mouse_y) : std::nullopt;
      const std::optional<SDL_FRect> current_close =
          hovered_tab_close(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      if (HoverChanged(previous_close, current_close)) {
        sidebar_hover_button_changed = true;
      }
    }
    const bool current_action_hovered =
        EditorHoverPopupPrimaryActionHovered(static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y));
    hover_visual_changed = !(previous_hover_target == active_editor_hover_target_) ||
                           previous_action_hovered != current_action_hovered;

    if (HoverChanged(previous_mouse_position_valid
                         ? HoveredStatusBarSegmentRect(layout, previous_mouse_x, previous_mouse_y)
                         : std::nullopt,
                     HoveredStatusBarSegmentRect(layout, static_cast<float>(event.motion.x),
                                                 static_cast<float>(event.motion.y)))) {
      status_segment_hover_changed = true;
    }
  }

  const bool chrome_tooltip_visual_changed = [&]() {
    const std::optional<HoverTooltip> current_tooltip = HoveredTooltip(layout);
    return previous_tooltip.has_value() != current_tooltip.has_value() ||
           (previous_tooltip.has_value() && (previous_tooltip->text != current_tooltip->text ||
                                             !RectsEqual(previous_tooltip->rect, current_tooltip->rect)));
  }();
  if (sidebar_hover_button_changed || status_segment_hover_changed || chrome_tooltip_visual_changed) {
    RequestPreparedFrame();
  }
  const DamageMark dispatch_mark = MarkDamage();
  if (MakeChromeMouseCoordinator().HandleMotion(event, layout)) {
    EnsureRedraw(dispatch_mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleMotion(event)) {
    EnsureRedraw(dispatch_mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (ActiveTabIsMerge()) {
    hover_visual_changed =
        hover_visual_changed ||
        MakeMergeMouseCoordinator().HandleHoverMotion(event, ActiveTabSurface(layout));
  }
  if (ActiveTabIsCompare()) {
    hover_visual_changed =
        hover_visual_changed ||
        MakeCompareMouseCoordinator().HandleHoverMotion(event, ActiveTabSurface(layout));
  }

  if (hover_visual_changed) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
  }

  return hover_visual_changed || chrome_tooltip_visual_changed || sidebar_hover_button_changed ||
         status_segment_hover_changed;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseWheel");
  const DamageMark mark = MarkDamage();
  if (context_.prompts.dirty_visible) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MenuSurfaceCapturingMouse()) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  UpdateMouseCursor(static_cast<float>(event.wheel.mouse_x),
                    static_cast<float>(event.wheel.mouse_y), false);

  const WheelTicks ticks = AccumulateWheelEvent(
      context_.interaction_state, event.wheel.y, event.wheel.x,
      event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
  const int vertical_ticks = ticks.vertical;
  const int horizontal_ticks =
      ticks.horizontal != 0
          ? ticks.horizontal
          : ((context_.interaction_state.key_modifiers & SDL_KMOD_SHIFT) != 0 ? vertical_ticks : 0);
  if (vertical_ticks == 0 && horizontal_ticks == 0) {
    return false;
  }

  const WorkspaceLayout layout = *layout_state;

  if (settings_overlay_service_.Visible()) {
    if (vertical_ticks != 0) {
      const SDL_FRect overlay_rect = ComputeSettingsOverlaySurfaceRect(layout.overlay_anchor, layout.full);
      if (Contains(overlay_rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
        const SettingsOverlayViewModel vm =
            RenderViewModelBuilder(context_)
                .BuildSettingsOverlay(layout, settings_overlay_service_, text_renderer_);
        if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
          if (vm.value_picker.visible &&
              Contains(vm.value_picker.rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
            settings_overlay_service_.SetPickerScroll(settings_overlay_service_.PickerScroll() -
                                                      vertical_ticks * kWheelScrollRows);
            EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
            return true;
          }
          if (Contains(vm.left_pane_rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
            settings_overlay_service_.SetCategoryScrollRow(
                std::clamp(settings_overlay_service_.CategoryScrollRow() -
                               vertical_ticks * kWheelScrollRows,
                           0, vm.category_max_scroll));
            EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
            return true;
          }
        }
        settings_overlay_service_.SetScrollRow(std::clamp(
            settings_overlay_service_.ScrollRow() - vertical_ticks * kWheelScrollRows, 0,
            vm.max_scroll));
      }
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakeChromeMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakeTabMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakeDebugPaneMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    auto& groups = context_.current_project_state.editor_groups;
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < groups.size(); ++gi) {
      const SDL_FRect& pane = group_rects.groups[gi].editor_surface;
      if (!Contains(pane, event.wheel.mouse_x, event.wheel.mouse_y) || !groups[gi].has_active_tab()) {
        continue;
      }
      TabEntry& tab = groups[gi].active_tab();
      bool handled = false;
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
        handled = MakeCompareMouseCoordinator().HandleWheel(event, pane, *tab.compare, vertical_ticks,
                                                            horizontal_ticks);
      } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
        handled = MakeMergeMouseCoordinator().HandleWheel(event, pane, *tab.merge, vertical_ticks,
                                                          horizontal_ticks);
      } else {
        break;
      }
      if (handled) {
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      }
      return handled;
    }
    const bool handled = MakeEditorMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
    if (handled) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return handled;
  }

  return false;
}

}
