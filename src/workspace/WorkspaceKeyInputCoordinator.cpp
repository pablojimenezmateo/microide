#include "workspace/WorkspaceKeyInputCoordinator.h"

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

KeyInputCoordinator::KeyInputCoordinator(WorkspaceShell& shell) : shell_(shell) {}

bool KeyInputCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!shell_.pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };

  const SDL_Keymod modifiers = event.mod != SDL_KMOD_NONE ? event.mod : SDL_GetModState();
  TextInputCoordinator text_input(shell_);
  if (shell_.prompts_.dirty_visible) {
    const bool handled = HandleDirtyPromptKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestPromptRedraw(); });
    }
    return handled;
  }
  if (shell_.menu_state_.tree_context_menu.open) {
    const bool handled = HandleTreeContextMenuKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestChromeRedraw(); });
    }
    return handled;
  }
  if (shell_.menu_state_.menu_bar_open) {
    const bool handled = HandleMenuBarKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestChromeRedraw(); });
    }
    return handled;
  }
  if (text_input.CompositionConsumesKey(event.key, modifiers)) {
    return true;
  }
  if (shell_.prompts_.surface_visible) {
    const bool handled = HandlePromptSurfaceKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestPromptRedraw(); });
    }
    return handled;
  }

  const bool active_compare_tab = shell_.ActiveTabIsCompare();
  const bool active_merge_tab = shell_.ActiveTabIsMerge();
  if (HandleGlobalKeyDown(event, modifiers, active_compare_tab, active_merge_tab)) {
    return true;
  }
  if (shell_.panel_state_.command_mode) {
    const bool handled = CommandPromptCoordinator(shell_).HandleKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestBottomPanelCommandRedraw(); });
    }
    return handled;
  }
  if (HandleSurfaceNavigationKeyDown(event, modifiers)) {
    ensure_redraw([this]() { shell_.RequestWindowRedraw(); });
    return true;
  }
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Overlay) {
    const bool handled = HandleOverlayKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestOverlayRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar &&
      shell_.sidebar_state_.visible) {
    const bool handled = HandleSidebarKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestSidebarRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Panel &&
      shell_.ActiveTerminalTab() != nullptr) {
    const bool handled = text_input.HandleTerminalKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestBottomPanelContentRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor && active_compare_tab) {
    const bool handled = HandleCompareKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestFocusedEditorRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor && active_merge_tab) {
    const bool handled = HandleMergeKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestFocusedEditorRedraw(); });
    }
    return handled;
  }

  const bool handled = HandleDefaultEditorKeyDown(event, modifiers);
  if (handled) {
    ensure_redraw([this]() { shell_.RequestFocusedEditorRedraw(); });
  }
  return handled;
}

bool KeyInputCoordinator::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers,
                                              bool active_compare_tab,
                                              bool active_merge_tab) {
  ActionCoordinator action(shell_);
  if ((modifiers & SDL_KMOD_CTRL) && !shell_.panel_state_.command_mode &&
      !shell_.overlay_state_.visible && event.key == SDLK_N) {
    return shell_.OpenUntitledTab();
  }

  if ((modifiers & SDL_KMOD_CTRL) && !shell_.panel_state_.command_mode &&
      !shell_.overlay_state_.visible &&
      shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor &&
      !active_compare_tab && shell_.ActiveEditableViewport() != nullptr && event.key == SDLK_A) {
    action.Execute(ActionId::SelectAll, {}, ActionSource::Shortcut);
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && !shell_.panel_state_.command_mode &&
      !shell_.overlay_state_.visible &&
      shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor &&
      !active_compare_tab) {
    if (!active_merge_tab && (modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_F) {
      action.Execute(ActionId::ProjectSearch, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key == SDLK_H) {
      action.Execute(ActionId::ReplaceInBuffer, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key == SDLK_F) {
      action.Execute(ActionId::Search, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_W) {
      action.Execute(ActionId::CloseActiveTab, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_Z) {
      action.Execute((modifiers & SDL_KMOD_SHIFT) != 0 ? ActionId::Redo : ActionId::Undo, {},
                     ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_Y) {
      action.Execute(ActionId::Redo, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_C) {
      action.Execute(ActionId::CopySelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_X) {
      action.Execute(ActionId::CutSelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_V) {
      action.Execute(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && !active_compare_tab && event.key == SDLK_S) {
    action.Execute(ActionId::Save, {}, ActionSource::Shortcut);
    return true;
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key == SDLK_0 || event.key == SDLK_KP_0) {
      action.Execute(ActionId::UiScale, {"reset"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_MINUS || event.key == SDLK_KP_MINUS) {
      action.Execute(ActionId::UiScale, {"down"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_EQUALS || event.key == SDLK_PLUS || event.key == SDLK_KP_PLUS) {
      action.Execute(ActionId::UiScale, {"up"}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_E) {
    action.Execute(ActionId::OpenCommandPrompt, {}, ActionSource::Shortcut);
    return true;
  }

  return false;
}

bool KeyInputCoordinator::HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event,
                                                         SDL_Keymod modifiers) {
  ActionCoordinator action(shell_);
  switch (event.key) {
    case SDLK_F8:
      action.Execute(ActionId::SidebarToggle, {}, ActionSource::Shortcut);
      return true;
    case SDLK_F6:
      action.Execute(ActionId::Files, {}, ActionSource::Shortcut);
      return true;
    case SDLK_TAB:
      if (modifiers & SDL_KMOD_CTRL) {
        if (shell_.overlay_state_.visible) {
          shell_.surface_.focus = WorkspaceShell::FocusTarget::Overlay;
          return true;
        }
        const bool include_panel = shell_.BottomPanelVisible();
        if (include_panel) {
          if (shell_.sidebar_state_.visible) {
            if (modifiers & SDL_KMOD_SHIFT) {
              shell_.surface_.focus =
                  shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar
                      ? WorkspaceShell::FocusTarget::Panel
                      : shell_.surface_.focus == WorkspaceShell::FocusTarget::Panel
                            ? WorkspaceShell::FocusTarget::Editor
                            : WorkspaceShell::FocusTarget::Sidebar;
            } else {
              shell_.surface_.focus =
                  shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar
                      ? WorkspaceShell::FocusTarget::Editor
                      : shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor
                            ? WorkspaceShell::FocusTarget::Panel
                            : WorkspaceShell::FocusTarget::Sidebar;
            }
          } else {
            shell_.surface_.focus =
                shell_.surface_.focus == WorkspaceShell::FocusTarget::Panel
                    ? WorkspaceShell::FocusTarget::Editor
                    : WorkspaceShell::FocusTarget::Panel;
          }
        } else if (shell_.sidebar_state_.visible && !(modifiers & SDL_KMOD_SHIFT)) {
          shell_.surface_.focus =
              shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar
                  ? WorkspaceShell::FocusTarget::Editor
                  : WorkspaceShell::FocusTarget::Sidebar;
        } else if (shell_.sidebar_state_.visible) {
          shell_.surface_.focus =
              shell_.surface_.focus == WorkspaceShell::FocusTarget::Editor
                  ? WorkspaceShell::FocusTarget::Sidebar
                  : WorkspaceShell::FocusTarget::Editor;
        } else {
          shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (shell_.overlay_state_.visible) {
        shell_.DismissOverlay();
        return true;
      }
      if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar &&
          shell_.sidebar_state_.visible &&
          shell_.sidebar_state_.temporary &&
          shell_.ActiveSidebarMode() == SidebarMode::Search) {
        shell_.CloseSidebar();
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

}  // namespace microide::workspace
