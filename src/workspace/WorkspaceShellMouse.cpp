#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceMergeMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
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

  interaction_state_.mouse_selecting = false;

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
    interaction_state_.drag_target = DragTarget::SidebarDivider;
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
    MenuCoordinator(*this).OpenAnchoredMenu(
        MenuId::Edit,
        MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                 1.0f));
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
  if (interaction_state_.drag_target != DragTarget::None) {
    ClearDragState();
    interaction_state_.mouse_selecting = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }
  const bool was_selecting = interaction_state_.mouse_selecting;
  interaction_state_.mouse_selecting = false;
  if (was_selecting) {
    SyncPrimarySelectionWithActiveEditor();
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return was_selecting;
}

}  // namespace microide::workspace
