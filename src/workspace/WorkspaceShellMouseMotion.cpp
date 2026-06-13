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
  if (context_.prompts.dirty_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;

  // Single-line input drag-select runs even when the modal prompt swallows other
  // motion handling, so the user can finish a selection started inside the prompt.
  if (context_.interaction_state.drag_target == DragTarget::SingleLineSelection) {
    if (HandleSingleLineInputDrag(event, layout)) {
      switch (context_.interaction_state.single_line_drag_surface) {
        case TextInputSurface::PromptInput:
          ensure_redraw([this]() { RequestPromptRedraw(); });
          break;
        case TextInputSurface::Command:
          ensure_redraw([this]() { RequestBottomPanelRedraw(); });
          break;
        case TextInputSurface::SidebarSearchQuery:
        case TextInputSurface::SidebarSearchReplace:
          ensure_redraw([this]() { RequestSidebarRedraw(); });
          break;
        default:
          ensure_redraw([this]() { RequestOverlayRedraw(); });
          break;
      }
      return true;
    }
  }

  // Settings scrollbar drag tracks the pointer regardless of where it moves.
  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar) {
    if (settings_overlay_service_.Visible() &&
        settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
      const SettingsOverlayViewModel vm =
          RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_);
      if (vm.scrollbar.has_value()) {
        settings_overlay_service_.SetScrollRow(std::clamp(
            static_cast<int>(std::lround(ScrollUnitsForPointer(
                *vm.scrollbar, static_cast<float>(event.motion.y),
                context_.interaction_state.drag_scrollbar_offset))),
            0, vm.max_scroll));
      }
    }
    ensure_redraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  if (context_.prompts.surface_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  const bool previous_mouse_position_valid = last_mouse_position_valid_;
  const float previous_mouse_x = last_mouse_x_;
  const float previous_mouse_y = last_mouse_y_;
  const auto capture_blocked_hover_visuals = [this, &layout]() {
    struct HoverVisuals {
      std::optional<SDL_FRect> project_tab_tooltip_rect;
      std::optional<SDL_FRect> tab_tooltip_rect;
      std::optional<SDL_FRect> status_tooltip_rect;
      std::optional<SDL_FRect> git_sidebar_tooltip_rect;
      std::optional<SDL_FRect> editor_hover_popup_rect;
    };

    HoverVisuals visuals{
        .project_tab_tooltip_rect = HoveredProjectTabTooltipRect(layout),
        .tab_tooltip_rect = HoveredTabTooltipRect(layout),
        .status_tooltip_rect = HoveredStatusTooltipRect(layout),
        .git_sidebar_tooltip_rect = HoveredGitSidebarTooltipRect(layout),
        .editor_hover_popup_rect = std::nullopt,
    };
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      visuals.editor_hover_popup_rect = popup->rect;
    }
    return visuals;
  };
  const auto invalidate_blocked_hover_visuals = [this](const auto& visuals) {
    const auto request = [this](const std::optional<SDL_FRect>& rect) {
      if (rect.has_value()) {
        RequestRedrawRect(*rect);
      }
    };
    request(visuals.project_tab_tooltip_rect);
    request(visuals.tab_tooltip_rect);
    request(visuals.status_tooltip_rect);
    request(visuals.git_sidebar_tooltip_rect);
    request(visuals.editor_hover_popup_rect);
  };
  const auto rects_equal = [](const SDL_FRect& lhs, const SDL_FRect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
  };
  const auto request_hover_button_redraw = [this](const SDL_FRect& rect) {
    RequestRedrawRect(MakeRect(rect.x - 1.0f, rect.y - 1.0f, rect.w + 2.0f, rect.h + 2.0f));
  };
  const auto sidebar_hover_button_rect_at =
      [this, &layout](float x, float y) -> std::optional<SDL_FRect> {
    if (!context_.current_project_state.sidebar.visible || MenuSurfaceCapturingMouse() ||
        !Contains(layout.sidebar, x, y)) {
      return std::nullopt;
    }

    const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
    for (int i = 0; i < mode_row.tab_count; ++i) {
      const SDL_FRect tab_rect = mode_row.tabs[static_cast<std::size_t>(i)].rect;
      if (Contains(tab_rect, x, y)) {
        return tab_rect;
      }
    }
    if (mode_row.has_overflow && Contains(mode_row.overflow_rect, x, y)) {
      return mode_row.overflow_rect;
    }

    switch (ActiveSidebarMode()) {
      case SidebarMode::Search: {
        const SDL_FRect mode_rect = ProjectSearchModeButtonRect(layout.sidebar);
        if (Contains(mode_rect, x, y)) {
          return mode_rect;
        }
        const SDL_FRect case_rect = ProjectSearchCaseButtonRect(layout.sidebar);
        if (Contains(case_rect, x, y)) {
          return case_rect;
        }
        const SDL_FRect hidden_rect = ProjectSearchHiddenButtonRect(layout.sidebar);
        if (Contains(hidden_rect, x, y)) {
          return hidden_rect;
        }
        return std::nullopt;
      }
      case SidebarMode::Git: {
        if (CanStageAllGitSidebarEntries()) {
          const SDL_FRect stage_rect = GitSidebarStageAllButtonRect(layout.sidebar);
          if (Contains(stage_rect, x, y)) {
            return stage_rect;
          }
        }
        if (CanDiscardAllGitSidebarEntries()) {
          const SDL_FRect discard_rect = GitSidebarDiscardAllButtonRect(layout.sidebar);
          if (Contains(discard_rect, x, y)) {
            return discard_rect;
          }
        }
        const SDL_FRect refresh_rect = GitSidebarRefreshButtonRect(layout.sidebar);
        if (Contains(refresh_rect, x, y)) {
          return refresh_rect;
        }
        if (const auto outgoing_base_rect = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
            outgoing_base_rect.has_value() && Contains(*outgoing_base_rect, x, y)) {
          return outgoing_base_rect;
        }
        // Per-row Stage / Discard buttons share this hover-invalidation path so their
        // hover state repaints on enter/leave like the header buttons above.
        if (const auto row_button = HoveredGitSidebarActionButtonRect(layout, x, y);
            row_button.has_value()) {
          return row_button;
        }
        return std::nullopt;
      }
      case SidebarMode::Tree: {
        if (context_.current_project_state.directory_tree.CanCollapseAll()) {
          const SDL_FRect collapse_rect = TreeSidebarCollapseButtonRect(layout.sidebar);
          if (Contains(collapse_rect, x, y)) {
            return collapse_rect;
          }
        }
        const SDL_FRect refresh_rect = TreeSidebarRefreshButtonRect(layout.sidebar);
        if (Contains(refresh_rect, x, y)) {
          return refresh_rect;
        }
        return std::nullopt;
      }
      case SidebarMode::None:
      case SidebarMode::Chat:
      case SidebarMode::Problems:
      case SidebarMode::Tests:
      case SidebarMode::Plugin:
        return std::nullopt;
    }

    return std::nullopt;
  };

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

    if (context_.interaction_state.drag_target == DragTarget::OverlayScrollbar &&
        context_.current_project_state.overlay.visible) {
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

  if (context_.interaction_state.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0) {
    if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
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

  if (MenuSurfaceCapturingMouse()) {
    static const bool menu_hover_trace =
        util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_MENU_HOVER");
    const Uint64 t0 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    const std::size_t rects_before =
        menu_hover_trace ? pending_render_invalidation_.rects.size() : 0;
    const auto blocked_hover_visuals = capture_blocked_hover_visuals();
    const Uint64 t1 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    const Uint64 t2 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    invalidate_blocked_hover_visuals(blocked_hover_visuals);
    const Uint64 t3 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    auto chrome_coordinator = MakeChromeMouseCoordinator();
    const Uint64 t4 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    chrome_coordinator.HandleMotion(event, layout);
    const Uint64 t5 = menu_hover_trace ? SDL_GetTicksNS() : 0;
    if (menu_hover_trace) {
      const auto ms = [](Uint64 a, Uint64 b) {
        return static_cast<double>(b - a) / 1'000'000.0;
      };
      SDL_Log("MICROIDE_MENU_HOVER motion ev=(%.0f,%.0f) total=%.3fms "
              "[capture=%.3f cursor=%.3f invalidate=%.3f makeCoord=%.3f handle=%.3f] "
              "rects+=%zu ami=%d hovered=%d",
              event.motion.x, event.motion.y, ms(t0, t5),
              ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5),
              pending_render_invalidation_.rects.size() - rects_before,
              context_.menu_state.active_menu_item_index,
              context_.menu_state.hovered_popup_row_index);
    }
    return true;
  }

  bool hover_visual_changed = false;
  bool sidebar_hover_button_changed = false;
  bool status_segment_hover_changed = false;
  std::optional<SDL_FRect> previous_project_tab_tooltip_rect;
  std::optional<SDL_FRect> previous_tab_tooltip_rect;
  std::optional<SDL_FRect> previous_status_tooltip_rect;
  std::optional<SDL_FRect> previous_sidebar_search_tooltip_rect;
  std::string previous_project_tab_tooltip_label;
  std::string previous_tab_tooltip_label;
  std::string previous_status_tooltip_label;
  std::string previous_sidebar_search_tooltip_label;
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
      previous_sidebar_search_tooltip_label =
          HoveredSidebarSearchTooltipLabel(layout_before_state->sidebar);
      previous_sidebar_search_tooltip_rect =
          HoveredSidebarSearchTooltipRect(*layout_before_state);
    }
    const std::optional<EditorHoverTarget> previous_hover_target = active_editor_hover_target_;
    const bool previous_action_hovered =
        last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::HandleMouseMotion::UpdateMouseCursor");
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    }
    const std::optional<SDL_FRect> previous_sidebar_hover_button_rect_optional =
        previous_mouse_position_valid
            ? sidebar_hover_button_rect_at(previous_mouse_x, previous_mouse_y)
            : std::nullopt;
    const std::optional<SDL_FRect> current_sidebar_hover_button_rect_optional =
        sidebar_hover_button_rect_at(static_cast<float>(event.motion.x),
                                     static_cast<float>(event.motion.y));
    const bool previous_has_button_rect = previous_sidebar_hover_button_rect_optional.has_value();
    const bool current_has_button_rect = current_sidebar_hover_button_rect_optional.has_value();
    SDL_FRect previous_button_rect = MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
    SDL_FRect current_button_rect = MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
    if (previous_has_button_rect) {
      previous_button_rect = *previous_sidebar_hover_button_rect_optional;
    }
    if (current_has_button_rect) {
      current_button_rect = *current_sidebar_hover_button_rect_optional;
    }
    sidebar_hover_button_changed =
        previous_has_button_rect != current_has_button_rect ||
        (previous_has_button_rect && !rects_equal(previous_button_rect, current_button_rect));
    if (sidebar_hover_button_changed) {
      if (previous_has_button_rect) {
        request_hover_button_redraw(previous_button_rect);
      }
      if (current_has_button_rect) {
        request_hover_button_redraw(current_button_rect);
      }
    }
    const bool current_action_hovered =
        EditorHoverPopupPrimaryActionHovered(static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y));
    hover_visual_changed = !(previous_hover_target == active_editor_hover_target_) ||
                           previous_action_hovered != current_action_hovered;

    // Clickable status-bar segments repaint their hover background on enter/leave. The
    // hover fill extends a few px past the text rect, so the redraw rect is padded wider.
    const std::optional<SDL_FRect> previous_status_segment_rect =
        previous_mouse_position_valid
            ? HoveredStatusBarSegmentRect(layout, previous_mouse_x, previous_mouse_y)
            : std::nullopt;
    const std::optional<SDL_FRect> current_status_segment_rect = HoveredStatusBarSegmentRect(
        layout, static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    if (previous_status_segment_rect.has_value() != current_status_segment_rect.has_value() ||
        (previous_status_segment_rect.has_value() &&
         !rects_equal(*previous_status_segment_rect, *current_status_segment_rect))) {
      const auto request_status_redraw = [this](const SDL_FRect& rect) {
        RequestRedrawRect(MakeRect(rect.x - 6.0f, rect.y - 1.0f, rect.w + 12.0f, rect.h + 2.0f));
      };
      if (previous_status_segment_rect.has_value()) {
        request_status_redraw(*previous_status_segment_rect);
      }
      if (current_status_segment_rect.has_value()) {
        request_status_redraw(*current_status_segment_rect);
      }
      status_segment_hover_changed = true;
    }
  }

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
                                        HoveredStatusTooltipRect(layout)) ||
      request_tooltip_redraw_if_changed(previous_sidebar_search_tooltip_label,
                                        previous_sidebar_search_tooltip_rect,
                                        HoveredSidebarSearchTooltipLabel(layout.sidebar),
                                        HoveredSidebarSearchTooltipRect(layout));
  if (MakeChromeMouseCoordinator().HandleMotion(event, layout)) {
    ensure_redraw([this]() { RequestChromeRedraw(); });
    return true;
  }

  if (context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    const bool handled = HandleTabMouseMotion(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
    }
    return handled;
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
  if (ActiveTabIsCompare()) {
    hover_visual_changed =
        hover_visual_changed ||
        MakeCompareMouseCoordinator().HandleHoverMotion(event, layout);
  }

  if (hover_visual_changed) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }

  return hover_visual_changed || chrome_tooltip_visual_changed || sidebar_hover_button_changed ||
         status_segment_hover_changed;
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

  if (MenuSurfaceCapturingMouse()) {
    ensure_redraw([this]() { RequestChromeRedraw(); });
    return true;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  UpdateMouseCursor(static_cast<float>(event.wheel.mouse_x),
                    static_cast<float>(event.wheel.mouse_y), false);

  // Fold the raw float wheel deltas into whole-line ticks via the shared
  // accumulator helper. Pulling this out keeps WorkspaceInteractionState
  // unit-testable without spinning up a full WorkspaceShell.
  const WheelTicks ticks = AccumulateWheelEvent(
      context_.interaction_state, event.wheel.y, event.wheel.x,
      event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
  const int vertical_ticks = ticks.vertical;
  const int horizontal_ticks =
      ticks.horizontal != 0
          ? ticks.horizontal
          : ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0 ? -vertical_ticks : 0);
  if (vertical_ticks == 0 && horizontal_ticks == 0) {
    return false;
  }

  const WorkspaceLayout layout = *layout_state;

  // The Settings / Help-About overlay is modal: while it is open the wheel
  // scrolls its row list (when the pointer is over the panel) and never reaches
  // the editor or sidebar underneath. Scrolling tracks whole entries.
  if (settings_overlay_service_.Visible()) {
    if (vertical_ticks != 0) {
      const SDL_FRect overlay_rect = ComputeSettingsOverlaySurfaceRect(layout.editor_area);
      if (Contains(overlay_rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
        // Settings rows are fixed-height, so the view model resolves the exact max
        // scroll; Help/About rows vary, so its bound is published from the render
        // pass into settings_overlay_max_scroll_row_.
        int max_scroll = settings_overlay_max_scroll_row_;
        if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
          max_scroll = RenderViewModelBuilder(context_)
                           .BuildSettingsOverlay(layout, settings_overlay_service_)
                           .max_scroll;
        }
        settings_overlay_service_.SetScrollRow(
            std::clamp(settings_overlay_service_.ScrollRow() - vertical_ticks, 0, max_scroll));
      }
    }
    ensure_redraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

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
