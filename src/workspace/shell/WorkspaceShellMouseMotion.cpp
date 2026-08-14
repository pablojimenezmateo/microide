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

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseMotion");
  if (context_.prompts.dirty_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw([this]() { RequestPromptRedraw(); });
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
      RequestSingleLineDragSurfaceRedraw(context_.interaction_state.single_line_drag_surface);
      return true;
    }
  }

  // Settings scrollbar drags track the pointer regardless of where it moves. The
  // row list scrolls in Help/About too; only the left rail is Settings-only, and
  // it self-gates by having no scrollbar in the other mode.
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
    EnsureRedraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  if (context_.prompts.surface_visible) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  // The Settings / Help-About overlay is modal. While it is open a plain hover can
  // change the scope-chip tooltip (and, when editing a font row, the picker
  // highlight), so repaint the overlay on motion. UpdateMouseCursor refreshes the
  // last-mouse position the tooltip/hover render reads. Cheap: the overlay repaints
  // wholesale and no underlying surface should react while it owns the screen.
  if (settings_overlay_service_.Visible()) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y), false);
    EnsureRedraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  const bool previous_mouse_position_valid = last_mouse_position_valid_;
  const float previous_mouse_x = last_mouse_x_;
  const float previous_mouse_y = last_mouse_y_;
  const auto capture_blocked_hover_visuals = [this, &layout]() {
    struct HoverVisuals {
      std::optional<SDL_FRect> tooltip_rect;
      std::optional<SDL_FRect> editor_hover_popup_rect;
    };

    HoverVisuals visuals;
    if (const auto tooltip = HoveredTooltip(layout); tooltip.has_value()) {
      visuals.tooltip_rect = tooltip->rect;
    }
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
    request(visuals.tooltip_rect);
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
        const SDL_FRect mode_rect = project_search_panel::ModeButtonRect(layout.sidebar);
        if (Contains(mode_rect, x, y)) {
          return mode_rect;
        }
        const SDL_FRect case_rect = project_search_panel::CaseButtonRect(layout.sidebar);
        if (Contains(case_rect, x, y)) {
          return case_rect;
        }
        const SDL_FRect hidden_rect = project_search_panel::HiddenButtonRect(layout.sidebar);
        if (Contains(hidden_rect, x, y)) {
          return hidden_rect;
        }
        const SDL_FRect scope_rect = project_search_panel::ScopeButtonRect(layout.sidebar);
        if (Contains(scope_rect, x, y)) {
          return scope_rect;
        }
        return std::nullopt;
      }
      case SidebarMode::Git: {
        if (CanStageAllGitSidebarEntries()) {
          const SDL_FRect stage_rect = git_sidebar_header::StageAllButtonRect(layout.sidebar);
          if (Contains(stage_rect, x, y)) {
            return stage_rect;
          }
        }
        if (CanDiscardAllGitSidebarEntries()) {
          const SDL_FRect discard_rect = git_sidebar_header::DiscardAllButtonRect(layout.sidebar);
          if (Contains(discard_rect, x, y)) {
            return discard_rect;
          }
        }
        const SDL_FRect branch_rect = git_sidebar_header::BranchButtonRect(layout.sidebar);
        if (Contains(branch_rect, x, y)) {
          return branch_rect;
        }
        const SDL_FRect sync_rect = git_sidebar_header::SyncButtonRect(layout.sidebar);
        if (Contains(sync_rect, x, y)) {
          return sync_rect;
        }
        const SDL_FRect refresh_rect = git_sidebar_header::RefreshButtonRect(layout.sidebar);
        if (Contains(refresh_rect, x, y)) {
          return refresh_rect;
        }
        if (const auto outgoing_base_rect = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
            outgoing_base_rect.has_value() && Contains(*outgoing_base_rect, x, y)) {
          return outgoing_base_rect;
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
      case SidebarMode::Problems:
      case SidebarMode::Tests:
      case SidebarMode::Plugin:
      case SidebarMode::Outline:
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

    if (context_.interaction_state.drag_target == DragTarget::RightPaneDivider) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      const float resolved_sidebar_width = context_.current_project_state.sidebar.visible
                                               ? context_.current_project_state.sidebar.width
                                               : 0.0f;
      // The pane hugs the right edge, so its width grows as the divider moves left.
      context_.current_project_state.debug_pane.width = ClampRightPaneWidth(
          window_width - static_cast<float>(event.motion.x), window_width, resolved_sidebar_width);
      MarkLayoutDirty();
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::EditorSplitDivider) {
      const SDL_FRect es = drag_layout.editor_surface;
      ProjectWorkspaceState& ps = context_.current_project_state;
      float fraction = ps.group_split_fraction;
      if (ps.group_split_orientation == EditorSplitOrientation::Horizontal && es.h > 0.0f) {
        fraction = (static_cast<float>(event.motion.y) - es.y) / es.h;
      } else if (es.w > 0.0f) {
        fraction = (static_cast<float>(event.motion.x) - es.x) / es.w;
      }
      ps.group_split_fraction = std::clamp(fraction, 0.1f, 0.9f);
      MarkLayoutDirty();
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::BottomPanelDivider) {
      if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
        RequestBottomPanelLayoutChangeRedraw(drag_layout);
        return true;
      }
    }

    if (MakePanelMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
      return true;
    }

    if (MakeMergeMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (context_.interaction_state.drag_target == DragTarget::SidebarScrollbar) {
      const bool handled = MakeSidebarMouseCoordinator().HandleDrag(event, drag_layout);
      if (handled) {
        EnsureRedraw([this]() { RequestSidebarRedraw(); });
      }
      return handled;
    }

    if (context_.interaction_state.drag_target == DragTarget::DebugPaneScrollbar) {
      const bool handled = MakeDebugPaneMouseCoordinator().HandleDrag(event, drag_layout);
      if (handled) {
        EnsureRedraw([this]() { RequestWindowRedraw(); });
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
      EnsureRedraw([this]() { RequestOverlayRedraw(); });
      return true;
    }

    if (MakeCompareMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    if (MakeEditorMouseCoordinator().HandleDrag(event, drag_layout)) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }

    ClearDragState();
    return false;
  }

  // A live tab drag owns the pointer, exactly like the divider/scrollbar drags
  // above it. It used to be handled at the very bottom of this function, which
  // meant every motion event first ran the whole hover pipeline — two tooltip
  // resolutions, two interactive-rect hit tests, the sidebar/status/floating
  // probes — for hover state that a drag cannot change, and then asked for a
  // FULL-WINDOW repaint. Nothing outside the dragged strip moves, so the damage
  // is that strip (padded for the ghost's drop shadow).
  if (context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    // The tab tooltip that was up when the gesture started is hidden for the whole
    // drag (HoveredTooltip refuses while dragging), so the frame that starts the
    // drag has to damage where it was or its card is left painted on the strip.
    const bool was_dragging = context_.interaction_state.tab_drag.dragging;
    const std::optional<HoverTooltip> tooltip_before =
        was_dragging ? std::nullopt : HoveredTooltip(layout);
    const bool handled = HandleTabMouseMotion(event, layout);
    if (handled) {
      EnsureRedraw([this]() {
        const auto strip = TabStripRectForKind(context_.interaction_state.tab_drag.kind,
                                               FocusedEditorGroupIndex());
        if (!strip.has_value()) {
          RequestWindowRedraw();
          return;
        }
        // The ghost paints a 1px-right / 2px-down drop shadow, so its damage runs
        // a couple of pixels past the strip it belongs to.
        constexpr float kGhostShadowPaddingPx = 3.0f;
        RequestRedrawRect(MakeRect(strip->x, strip->y, strip->w + kGhostShadowPaddingPx,
                                   strip->h + kGhostShadowPaddingPx));
      });
      if (!was_dragging && context_.interaction_state.tab_drag.dragging &&
          tooltip_before.has_value()) {
        const SDL_FRect& card = tooltip_before->rect;
        RequestRedrawRect(MakeRect(card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f));
      }
    }
    return handled;
  }

  if (context_.interaction_state.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0) {
    // Deliberately NOT gated on the pointer being inside `layout.editor_surface`.
    // It used to be, which is why a selection drag stopped extending the moment
    // the pointer crossed into the tab strip, the sidebar, the bottom panel or
    // off the window -- and stayed stopped while the button was still down. The
    // surface handlers below clamp the pointer onto their own visible text band,
    // so "outside" is a position to project, not a reason to refuse.
    //
    // The cursor is not updated here either: during a drag the pointer is
    // logically still on text wherever it physically is, so re-resolving the
    // cursor kind per motion event would both flicker it over other surfaces and
    // do a hit-test this path has no use for.
    if (ActiveTabIsCompare()) {
      const bool handled = MakeCompareMouseCoordinator().HandleSelectionMotion(event, layout);
      if (handled) {
        EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }

    if (ActiveTabIsMerge()) {
      const bool handled = MakeMergeMouseCoordinator().HandleSelectionMotion(event, layout);
      if (handled) {
        EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }

    const bool handled = MakeEditorMouseCoordinator().HandleSelectionMotion(event, layout);
    if (handled) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
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
  std::optional<HoverTooltip> previous_tooltip;
  if (CurrentWindowRect().has_value()) {
    // The "before" layout is geometrically identical to `layout` (nothing above
    // mutates a layout input on this fall-through path); the "previous" aspect comes
    // from the not-yet-updated last_mouse_ position HoveredTooltip reads. Reuse
    // the already-computed layout instead of a second full ComputeLayout pass.
    previous_tooltip = HoveredTooltip(layout);
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

    // Generic hover-background surfaces (menu-bar items, tab strips, sidebar/overlay
    // list rows, bottom-panel tabs, debug-pane rows). Rendering is surface-granular,
    // so damaging the changed element's rect re-renders that surface with the new
    // hover. One unified probe keeps every list/tab hover affordance in lockstep.
    const std::optional<SDL_FRect> previous_interactive_rect =
        previous_mouse_position_valid
            ? HoveredInteractiveRect(layout, previous_mouse_x, previous_mouse_y)
            : std::nullopt;
    const std::optional<SDL_FRect> current_interactive_rect = HoveredInteractiveRect(
        layout, static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    const bool previous_interactive_present = previous_interactive_rect.has_value();
    const bool current_interactive_present = current_interactive_rect.has_value();
    const SDL_FRect previous_interactive = previous_interactive_rect.value_or(SDL_FRect{});
    const SDL_FRect current_interactive = current_interactive_rect.value_or(SDL_FRect{});
    if (previous_interactive_present != current_interactive_present ||
        (previous_interactive_present && current_interactive_present &&
         !rects_equal(previous_interactive, current_interactive))) {
      if (previous_interactive_present) {
        RequestRedrawRect(previous_interactive);
      }
      if (current_interactive_present) {
        RequestRedrawRect(current_interactive);
      }
      sidebar_hover_button_changed = true;  // reuse the "redraw was requested" return signal
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
    // value_or yields fully-initialized locals so the rect comparison/redraw paths never
    // touch optional storage GCC cannot prove engaged (the values matter only when the
    // matching has_value() flag below is true).
    const bool previous_status_segment_present = previous_status_segment_rect.has_value();
    const bool current_status_segment_present = current_status_segment_rect.has_value();
    const SDL_FRect previous_status_segment = previous_status_segment_rect.value_or(SDL_FRect{});
    const SDL_FRect current_status_segment = current_status_segment_rect.value_or(SDL_FRect{});
    if (previous_status_segment_present != current_status_segment_present ||
        (previous_status_segment_present && current_status_segment_present &&
         !rects_equal(previous_status_segment, current_status_segment))) {
      const auto request_status_redraw = [this](const SDL_FRect& rect) {
        RequestRedrawRect(MakeRect(rect.x - 6.0f, rect.y - 1.0f, rect.w + 12.0f, rect.h + 2.0f));
      };
      if (previous_status_segment_present) {
        request_status_redraw(previous_status_segment);
      }
      if (current_status_segment_present) {
        request_status_redraw(current_status_segment);
      }
      status_segment_hover_changed = true;
    }

    // Floating widgets whose buttons highlight under the pointer (the debug
    // toolbar and the two find widgets): repaint the card on hover enter/leave so
    // the highlight tracks the pointer like the rest of the chrome does. `-1` is
    // "off the card", `-2` "on the card but between buttons".
    const auto floating_widget_hover_changed =
        [&](const SDL_FRect& card, std::span<const SDL_FRect> buttons) {
          const auto hovered_button = [&](float px, float py) -> int {
            if (!Contains(card, px, py)) {
              return -1;
            }
            for (std::size_t i = 0; i < buttons.size(); ++i) {
              if (Contains(buttons[i], px, py)) {
                return static_cast<int>(i);
              }
            }
            return -2;
          };
          const int previous_button = previous_mouse_position_valid
                                          ? hovered_button(previous_mouse_x, previous_mouse_y)
                                          : -1;
          const int current_button = hovered_button(static_cast<float>(event.motion.x),
                                                    static_cast<float>(event.motion.y));
          if (previous_button == current_button ||
              (previous_button == -1 && current_button == -1)) {
            return false;
          }
          RequestRedrawRect(
              MakeRect(card.x - 2.0f, card.y - 2.0f, card.w + 4.0f, card.h + 4.0f));
          return true;
        };

    if (DebugToolbarVisible()) {
      const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
          layout.editor_surface, DebugToolbarAvoidBelowY(layout), DebugSupportsReverse());
      hover_visual_changed =
          floating_widget_hover_changed(tb.widget,
                                        std::span<const SDL_FRect>(tb.buttons.data(),
                                                                   tb.button_count)) ||
          hover_visual_changed;
    }

    // Both find widgets: same layout, same buttons, so one collector serves them.
    const auto find_widget_hover_changed = [&](const FindWidgetLayout& fw) {
      std::array<SDL_FRect, kFindWidgetMaxToggles + 5> buttons{};
      std::size_t count = 0;
      for (std::size_t i = 0; i < fw.toggle_count; ++i) {
        buttons[count++] = fw.toggle_buttons[i];
      }
      buttons[count++] = fw.prev_button;
      buttons[count++] = fw.next_button;
      buttons[count++] = fw.close_button;
      if (fw.replace_mode) {
        buttons[count++] = fw.replace_button;
        buttons[count++] = fw.replace_all_button;
      }
      return floating_widget_hover_changed(fw.widget,
                                           std::span<const SDL_FRect>(buttons.data(), count));
    };

    const OverlayMode overlay_mode = context_.current_project_state.overlay.mode;
    if (context_.current_project_state.overlay.visible &&
        (overlay_mode == OverlayMode::BufferSearch || overlay_mode == OverlayMode::BufferReplace)) {
      hover_visual_changed =
          find_widget_hover_changed(ComputeBufferFindWidgetLayout(
              layout.editor_surface, overlay_mode == OverlayMode::BufferReplace)) ||
          hover_visual_changed;
    }
    if (BottomPanelVisible() && terminal_find_service_.visible()) {
      hover_visual_changed =
          find_widget_hover_changed(ComputeFindWidgetLayout(BottomPanelContentRect(layout),
                                                            /*replace_mode=*/false,
                                                            kTerminalFindToggleCount)) ||
          hover_visual_changed;
    }
  }

  // One tooltip can show at a time, so one before/after comparison covers every
  // provider: repaint the card that is leaving and the card that is arriving.
  const bool chrome_tooltip_visual_changed = [&]() {
    const std::optional<HoverTooltip> current_tooltip = HoveredTooltip(layout);
    const bool unchanged =
        previous_tooltip.has_value() == current_tooltip.has_value() &&
        (!previous_tooltip.has_value() ||
         (previous_tooltip->text == current_tooltip->text &&
          SameTooltipRect(previous_tooltip->rect, current_tooltip->rect)));
    if (unchanged) {
      return false;
    }
    static constexpr float kTooltipRedrawPaddingPx = 1.0f;
    const auto request_padded = [this](const SDL_FRect& rect) {
      RequestRedrawRect(MakeRect(rect.x - kTooltipRedrawPaddingPx, rect.y - kTooltipRedrawPaddingPx,
                                 rect.w + 2.0f * kTooltipRedrawPaddingPx,
                                 rect.h + 2.0f * kTooltipRedrawPaddingPx));
    };
    if (previous_tooltip.has_value()) {
      request_padded(previous_tooltip->rect);
    }
    if (current_tooltip.has_value()) {
      request_padded(current_tooltip->rect);
    }
    return true;
  }();
  if (MakeChromeMouseCoordinator().HandleMotion(event, layout)) {
    EnsureRedraw([this]() { RequestChromeRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleMotion(event)) {
    EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
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
    EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
  }

  return hover_visual_changed || chrome_tooltip_visual_changed || sidebar_hover_button_changed ||
         status_segment_hover_changed;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseWheel");
  if (context_.prompts.dirty_visible) {
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (MenuSurfaceCapturingMouse()) {
    EnsureRedraw([this]() { RequestChromeRedraw(); });
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
        // Both modes resolve their (variable-height) row geometry in the builder,
        // so the exact max scroll comes from the view model. Help/About used to
        // read a bound published out of the render pass, which meant the wheel did
        // nothing until a paint had happened and could act on a stale bound after a
        // resize.
        const SettingsOverlayViewModel vm =
            RenderViewModelBuilder(context_)
                .BuildSettingsOverlay(layout, settings_overlay_service_, text_renderer_);
        if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
          // While the font picker is open, the wheel scrolls the dropdown when the
          // pointer is over it, leaving the rows beneath untouched.
          if (vm.value_picker.visible &&
              Contains(vm.value_picker.rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
            settings_overlay_service_.SetPickerScroll(settings_overlay_service_.PickerScroll() -
                                                      vertical_ticks * kWheelScrollRows);
            EnsureRedraw([this]() { RequestOverlayRedraw(); });
            return true;
          }
          // Over the left rail: scroll the category list instead of the value rows.
          if (Contains(vm.left_pane_rect, event.wheel.mouse_x, event.wheel.mouse_y)) {
            settings_overlay_service_.SetCategoryScrollRow(
                std::clamp(settings_overlay_service_.CategoryScrollRow() -
                               vertical_ticks * kWheelScrollRows,
                           0, vm.category_max_scroll));
            EnsureRedraw([this]() { RequestOverlayRedraw(); });
            return true;
          }
        }
        settings_overlay_service_.SetScrollRow(std::clamp(
            settings_overlay_service_.ScrollRow() - vertical_ticks * kWheelScrollRows, 0,
            vm.max_scroll));
      }
    }
    EnsureRedraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  if (MakeChromeMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks)) {
    EnsureRedraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (HandleTabMouseWheel(event, layout, vertical_ticks)) {
    EnsureRedraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  if (MakeDebugPaneMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw([this]() { RequestDebugPaneRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleWheel(event, layout, vertical_ticks)) {
    EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      const bool handled =
          MakeCompareMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    if (ActiveTabIsMerge()) {
      const bool handled =
          MakeMergeMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
      if (handled) {
        EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      }
      return handled;
    }
    const bool handled = MakeEditorMouseCoordinator().HandleWheel(event, layout, vertical_ticks, horizontal_ticks);
    if (handled) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  return false;
}

}  // namespace microide::workspace
