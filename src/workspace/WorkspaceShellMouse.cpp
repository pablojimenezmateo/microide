#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspaceMergeMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/WorkspaceShellShared.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE &&
      event.button.button != SDL_BUTTON_RIGHT) {
    return false;
  }
  const auto window_rect = CurrentWindowRect();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!window_rect.has_value() || !layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;

  const auto visible_hover_popup = ActiveEditorHoverPopupLayout();
  if (event.button.button == SDL_BUTTON_LEFT && visible_hover_popup.has_value() &&
      Contains(visible_hover_popup->rect, event.button.x, event.button.y)) {
    if (visible_hover_popup->kind == EditorHoverTarget::Kind::Blame &&
        visible_hover_popup->primary_action_rect.has_value() &&
        Contains(EditorHoverPopupPrimaryActionHitRect(*visible_hover_popup), event.button.x,
                 event.button.y)) {
      if (const editor::EditorBlameLine* blame_line =
              VisibleEditorBlameLine(visible_hover_popup->blame_line_index);
          blame_line != nullptr && !blame_line->commit_id.empty() &&
          WriteClipboardText(blame_line->commit_id)) {
      }
    }
    surface_.focus = FocusTarget::Editor;
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    return true;
  }
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (prompts_.dirty_visible) {
    const SDL_FRect dialog = ComputeDirtyPromptRect(*window_rect);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompts_.dirty.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        ensure_redraw([this]() { RequestPromptRedraw(); });
        return true;
      }
    }
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (prompts_.surface_visible) {
    const SDL_FRect dialog = ComputePromptSurfaceRect(*window_rect);
    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompts_.surface.selected_button = static_cast<int>(i);
        if (event.button.button == SDL_BUTTON_LEFT) {
          ConfirmPromptSurface();
        }
        ensure_redraw([this]() { RequestPromptRedraw(); });
        return true;
      }
    }
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (!text_composition_.text.empty()) {
    text_composition_ = TextCompositionState{};
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) {
      SDL_ClearComposition(window);
    }
  }

  surface_.mouse_selecting = false;

  if (ChromeMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (EditorBlameLineAtPosition(static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y)) != nullptr) {
      surface_.focus = FocusTarget::Editor;
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && surface_.sidebar_visible &&
      Contains(SidebarResizeHandleRect(layout), event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleResizeButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (SidebarMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  if (TabMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_RIGHT &&
      Contains(layout.editor_surface, event.button.x, event.button.y) &&
      ActiveEditableViewport() != nullptr) {
    OpenAnchoredMenu(MenuId::Edit,
                     MakeRect(static_cast<float>(event.button.x),
                              static_cast<float>(event.button.y), 1.0f, 1.0f));
    surface_.focus = FocusTarget::Editor;
    ensure_redraw([this]() { RequestChromeRedraw(); });
    return true;
  }

  if (!Contains(layout.editor_surface, event.button.x, event.button.y) ||
      (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    const bool handled = CompareMouseCoordinator(*this).HandleButtonDown(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge()) {
    const bool handled = MergeMouseCoordinator(*this).HandleButtonDown(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  const bool handled = EditorMouseCoordinator(*this).HandleButtonDown(event, layout);
  if (handled) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return handled;
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (prompts_.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (prompts_.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && tab_drag_state_.kind != TabDragKind::None) {
    if (TabMouseCoordinator(*this).HandleButtonUp(event)) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
      return true;
    }
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleButtonUp(event)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (surface_.drag_target != DragTarget::None) {
    surface_.drag_target = DragTarget::None;
    surface_.drag_scrollbar_offset = 0.0f;
    surface_.drag_editor_split_path.clear();
    surface_.drag_editor_split_divider_index = 0;
    surface_.mouse_selecting = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }
  const bool was_selecting = surface_.mouse_selecting;
  surface_.mouse_selecting = false;
  if (was_selecting) {
    SyncPrimarySelectionWithActiveEditor();
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return was_selecting;
}

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
      const float window_width =
          CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      surface_.sidebar_width =
          ClampSidebarWidth(static_cast<float>(event.motion.x), window_width);
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

  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
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
