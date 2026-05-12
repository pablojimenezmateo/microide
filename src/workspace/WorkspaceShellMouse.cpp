#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>

#include "editor/EditorViewRenderer.h"
#include "util/PerformanceTrace.h"
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
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonDown");
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
  const auto invalidate_menu_blocked_hover_visuals = [this, &layout]() {
    if (const auto rect = HoveredProjectTabTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredTabTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredStatusTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredGitSidebarTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      RequestRedrawRect(popup->rect);
    }
  };

  if (MenuSurfaceCapturingMouse()) {
    invalidate_menu_blocked_hover_visuals();
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
    if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
      ensure_redraw([this]() { RequestChromeRedraw(); });
      return true;
    }
    ensure_redraw([this]() { RequestChromeRedraw(); });
    return true;
  }

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
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    return true;
  }
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (context_.prompts.dirty_visible) {
    const SDL_FRect dialog = ComputeDirtyPromptRect(*window_rect);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        context_.prompts.dirty.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        ensure_redraw([this]() { RequestPromptRedraw(); });
        return true;
      }
    }
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (context_.prompts.surface_visible) {
    const SDL_FRect dialog = ComputePromptSurfaceRect(*window_rect);
    const auto buttons =
        ComputePromptSurfaceButtonRects(dialog, context_.prompts.surface.button_count);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        context_.prompts.surface.selected_button = static_cast<int>(i);
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

  if (!context_.text_input.composition.text.empty()) {
    context_.text_input.composition = TextCompositionState{};
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) {
      SDL_ClearComposition(window);
    }
  }

  context_.interaction_state.mouse_selecting = false;

  if (HandleSettingsOverlayButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  const auto blocked_project_tab_tooltip_rect = HoveredProjectTabTooltipRect(layout);
  const auto blocked_tab_tooltip_rect = HoveredTabTooltipRect(layout);
  const auto blocked_status_tooltip_rect = HoveredStatusTooltipRect(layout);
  const auto blocked_git_sidebar_tooltip_rect = HoveredGitSidebarTooltipRect(layout);
  const auto blocked_editor_hover_popup_rect = [&]() -> std::optional<SDL_FRect> {
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      return popup->rect;
    }
    return std::nullopt;
  }();
  if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
    if (MenuSurfaceCapturingMouse()) {
      if (blocked_project_tab_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_project_tab_tooltip_rect);
      }
      if (blocked_tab_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_tab_tooltip_rect);
      }
      if (blocked_status_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_status_tooltip_rect);
      }
      if (blocked_git_sidebar_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_git_sidebar_tooltip_rect);
      }
      if (blocked_editor_hover_popup_rect.has_value()) {
        RequestRedrawRect(*blocked_editor_hover_popup_rect);
      }
      UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
      ensure_redraw([this]() { RequestChromeRedraw(); });
    } else {
      ensure_redraw([this]() { RequestWindowRedraw(); });
    }
    return true;
  }

  if (HandleStatusBarButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (EditorBlameLineAtPosition(static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y)) != nullptr) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.current_project_state.sidebar.visible &&
      Contains(SidebarResizeHitRect(layout), event.button.x, event.button.y)) {
    context_.interaction_state.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (MakePanelMouseCoordinator().HandleResizeButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  {
    util::PerformanceTrace::Scope tab_scope("WorkspaceShell::HandleMouseButtonDown::Tabs");
    if (HandleTabMouseButtonDown(event, layout)) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
      return true;
    }
  }

  if (MakePanelMouseCoordinator().HandleButtonDown(event, layout)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_RIGHT &&
      Contains(layout.editor_surface, event.button.x, event.button.y) &&
      ActiveEditableViewport() != nullptr) {
    const bool retargeted_cursor = [&]() {
      if (!ActiveTabIsEditor()) {
        return false;
      }

      SyncActiveEditorTab();
      auto* editor_tab = ActiveEditorTab();
      if (editor_tab == nullptr) {
        return false;
      }
      NormalizeEditorSplitTree(*editor_tab);

      const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
      const auto pane_it = std::find_if(
          panes.begin(), panes.end(),
          [&](const EditorPaneLayout& pane) {
            return Contains(pane.rect, event.button.x, event.button.y);
          });
      if (pane_it == panes.end()) {
        return false;
      }
      if (!pane_it->active) {
        SetActiveEditorSplit(pane_it->leaf_id);
      }

      editor::TextViewport* viewport = ActiveEditorViewport();
      if (viewport == nullptr) {
        return false;
      }

      const editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane_it->rect);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);

      const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
      const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
      const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
      const std::size_t visual_column =
          viewport->horizontal_scroll() +
          static_cast<std::size_t>(std::max(
              0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));
      const int visual_row = static_cast<int>(viewport->scroll_line() + row);
      const editor::LogicalPosition hit =
          viewport->LogicalPositionForVisualHit(visual_row, static_cast<int>(visual_column));
      viewport->MoveCursorToVisualColumn(hit.line, visual_column, false);
      return true;
    }();
    MakeMenuCoordinator().OpenAnchoredMenu(
        MenuId::EditorContext,
        MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                 1.0f));
    if (retargeted_cursor) {
      ResetCaretBlink();
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    ensure_redraw([this, retargeted_cursor]() {
      RequestChromeRedraw();
      if (retargeted_cursor) {
        RequestFocusedEditorRedraw();
      }
    });
    return true;
  }

  if (!Contains(layout.editor_surface, event.button.x, event.button.y) ||
      (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    const bool handled = MakeCompareMouseCoordinator().HandleButtonDown(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge()) {
    const bool handled = MakeMergeMouseCoordinator().HandleButtonDown(event, layout);
    if (handled) {
      ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  util::PerformanceTrace::Scope editor_scope("WorkspaceShell::HandleMouseButtonDown::Editor");
  const bool handled = MakeEditorMouseCoordinator().HandleButtonDown(event, layout);
  if (handled) {
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return handled;
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonUp");
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (context_.prompts.dirty_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    ensure_redraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    if (HandleTabMouseButtonUp(event)) {
      ensure_redraw([this]() { RequestWindowRedraw(); });
      return true;
    }
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleButtonUp(event)) {
    ensure_redraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (context_.interaction_state.drag_target != DragTarget::None) {
    ClearDragState();
    context_.interaction_state.mouse_selecting = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return true;
  }
  const bool was_selecting = context_.interaction_state.mouse_selecting;
  context_.interaction_state.mouse_selecting = false;
  if (was_selecting) {
    SyncPrimarySelectionWithActiveEditor();
    ensure_redraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return was_selecting;
}

}  // namespace microide::workspace
