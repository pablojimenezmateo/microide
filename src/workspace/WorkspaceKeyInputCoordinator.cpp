#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <limits>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

WorkspaceShell::KeyInputCoordinator::KeyInputCoordinator(WorkspaceShell& shell) : shell_(shell) {}

bool WorkspaceShell::KeyInputCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!shell_.pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };

  const SDL_Keymod modifiers =
      event.mod != SDL_KMOD_NONE ? event.mod : SDL_GetModState();
  TextInputCoordinator text_input(shell_);
  if (shell_.prompts_.dirty_visible) {
    const bool handled = HandleDirtyPromptKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestPromptRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.tree_context_menu.open) {
    const bool handled = HandleTreeContextMenuKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestChromeRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.menu_bar_open) {
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
  if (shell_.surface_.command_mode) {
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
  if (shell_.surface_.focus == FocusTarget::Overlay) {
    const bool handled = HandleOverlayKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestOverlayRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == FocusTarget::Sidebar && shell_.surface_.sidebar_visible) {
    const bool handled = HandleSidebarKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestSidebarRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == FocusTarget::Panel && shell_.ActiveTerminalTab() != nullptr) {
    const bool handled = text_input.HandleTerminalKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestBottomPanelContentRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == FocusTarget::Editor && active_compare_tab) {
    const bool handled = HandleCompareKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { shell_.RequestFocusedEditorRedraw(); });
    }
    return handled;
  }
  if (shell_.surface_.focus == FocusTarget::Editor && active_merge_tab) {
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

bool WorkspaceShell::KeyInputCoordinator::HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers) {
  (void) modifiers;
  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.prompts_.dirty.selected_action = 2;
      shell_.ConfirmDirtyPrompt();
      return true;
    case SDLK_LEFT:
      shell_.prompts_.dirty.selected_action = std::max(0, shell_.prompts_.dirty.selected_action - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      shell_.prompts_.dirty.selected_action = std::min(2, shell_.prompts_.dirty.selected_action + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.ConfirmDirtyPrompt();
      return true;
    default: {
      const char input_character = shell_.KeycodeToAscii(event.key, SDL_GetModState());
      if (input_character == 's') {
        shell_.prompts_.dirty.selected_action = 0;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'd') {
        shell_.prompts_.dirty.selected_action = 1;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'c') {
        shell_.prompts_.dirty.selected_action = 2;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      return true;
    }
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleTreeContextMenuKeyDown(const SDL_KeyboardEvent& event) {
  MenuCoordinator menu(shell_);
  switch (event.key) {
    case SDLK_ESCAPE:
      menu.CloseTreeContextMenu();
      return true;
    case SDLK_DOWN:
      shell_.surface_.tree_context_menu.active_item_index =
          menu.NextEnabledTreeContextMenuItemIndex(shell_.surface_.tree_context_menu.active_item_index, 1);
      return true;
    case SDLK_UP:
      shell_.surface_.tree_context_menu.active_item_index =
          menu.NextEnabledTreeContextMenuItemIndex(shell_.surface_.tree_context_menu.active_item_index,
                                                   -1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (shell_.surface_.tree_context_menu.active_item_index >= 0) {
        return menu.ExecuteTreeContextMenuItem(
            static_cast<std::size_t>(shell_.surface_.tree_context_menu.active_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleMenuBarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  MenuCoordinator menu(shell_);
  switch (event.key) {
    case SDLK_ESCAPE:
      menu.CloseMenuBar();
      return true;
    case SDLK_LEFT:
      return menu.SwitchMenuBarMenu(-1);
    case SDLK_RIGHT:
      return menu.SwitchMenuBarMenu(1);
    case SDLK_TAB:
      return menu.SwitchMenuBarMenu((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    case SDLK_DOWN:
      return menu.MoveActiveMenuItem(1);
    case SDLK_UP:
      return menu.MoveActiveMenuItem(-1);
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (shell_.surface_.active_menu_item_index >= 0) {
        return menu.ExecuteMenuItem(shell_.surface_.active_menu_id,
                                    static_cast<std::size_t>(shell_.surface_.active_menu_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event) {
  if (shell_.prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.DismissPromptSurface(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        shell_.prompts_.surface.selected_button = 0;
        shell_.ConfirmPromptSurface();
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&shell_.prompts_.surface.input);
        return true;
      default:
        return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.DismissPromptSurface(true);
      return true;
    case SDLK_LEFT:
      shell_.prompts_.surface.selected_button = std::max(0, shell_.prompts_.surface.selected_button - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      shell_.prompts_.surface.selected_button = std::min(1, shell_.prompts_.surface.selected_button + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.ConfirmPromptSurface();
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                         SDL_Keymod modifiers,
                                         bool active_compare_tab,
                                         bool active_merge_tab) {
  ActionCoordinator action(shell_);
  if ((modifiers & SDL_KMOD_CTRL) && !shell_.surface_.command_mode && !shell_.surface_.overlay_visible &&
      event.key == SDLK_N) {
    return shell_.OpenUntitledTab();
  }

  if ((modifiers & SDL_KMOD_CTRL) && !shell_.surface_.command_mode && !shell_.surface_.overlay_visible &&
      shell_.surface_.focus == FocusTarget::Editor && !active_compare_tab &&
      shell_.ActiveEditableViewport() != nullptr && event.key == SDLK_A) {
    action.Execute(ActionId::SelectAll, {}, ActionSource::Shortcut);
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && !shell_.surface_.command_mode && !shell_.surface_.overlay_visible &&
      shell_.surface_.focus == FocusTarget::Editor && !active_compare_tab) {
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

bool WorkspaceShell::KeyInputCoordinator::HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event,
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
        if (shell_.surface_.overlay_visible) {
          shell_.surface_.focus = FocusTarget::Overlay;
          return true;
        }
        const bool include_panel = shell_.BottomPanelVisible();
        if (include_panel) {
          if (shell_.surface_.sidebar_visible) {
            if (modifiers & SDL_KMOD_SHIFT) {
              shell_.surface_.focus = shell_.surface_.focus == FocusTarget::Sidebar
                                   ? FocusTarget::Panel
                                   : shell_.surface_.focus == FocusTarget::Panel
                                         ? FocusTarget::Editor
                                         : FocusTarget::Sidebar;
            } else {
              shell_.surface_.focus = shell_.surface_.focus == FocusTarget::Sidebar
                                   ? FocusTarget::Editor
                                   : shell_.surface_.focus == FocusTarget::Editor
                                         ? FocusTarget::Panel
                                         : FocusTarget::Sidebar;
            }
          } else {
            shell_.surface_.focus =
                shell_.surface_.focus == FocusTarget::Panel ? FocusTarget::Editor : FocusTarget::Panel;
          }
        } else if (shell_.surface_.sidebar_visible && !(modifiers & SDL_KMOD_SHIFT)) {
          shell_.surface_.focus =
              shell_.surface_.focus == FocusTarget::Sidebar ? FocusTarget::Editor : FocusTarget::Sidebar;
        } else if (shell_.surface_.sidebar_visible) {
          shell_.surface_.focus =
              shell_.surface_.focus == FocusTarget::Editor ? FocusTarget::Sidebar : FocusTarget::Editor;
        } else {
          shell_.surface_.focus = FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (shell_.surface_.overlay_visible) {
        shell_.DismissOverlay();
        return true;
      }
      if (shell_.surface_.focus == FocusTarget::Sidebar && shell_.surface_.sidebar_visible &&
          shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_mode == SidebarMode::Search) {
        shell_.CloseSidebar();
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

bool WorkspaceShell::KeyInputCoordinator::HandleOverlayKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  if (shell_.surface_.overlay_mode == OverlayMode::CommitPicker) {
    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.DismissOverlay();
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return shell_.ActivateOverlaySelection();
      case SDLK_UP:
        shell_.MoveComparePickerSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveComparePickerSelection(1);
        return true;
      case SDLK_HOME:
        if (!shell_.overlay_workflow_.compare_picker.matches.empty()) {
          shell_.overlay_workflow_.compare_picker.selected_index = 0;
          if (const auto layout = shell_.CurrentWorkspaceLayout(); layout.has_value()) {
            shell_.RevealOverlaySelection(shell_.ComputeOverlayRect(layout->editor_area));
          }
        }
        return true;
      case SDLK_END:
        if (!shell_.overlay_workflow_.compare_picker.matches.empty()) {
          shell_.overlay_workflow_.compare_picker.selected_index =
              shell_.overlay_workflow_.compare_picker.matches.size() - 1;
          if (const auto layout = shell_.CurrentWorkspaceLayout(); layout.has_value()) {
            shell_.RevealOverlaySelection(shell_.ComputeOverlayRect(layout->editor_area));
          }
        }
        return true;
      case SDLK_PAGEUP:
        shell_.MoveComparePickerSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveComparePickerSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.compare_picker.query)) {
          shell_.RefreshComparePicker();
        }
        return true;
      default:
        return false;
    }
  }

  if (shell_.surface_.overlay_mode == OverlayMode::BufferSearch) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return shell_.ActivateOverlaySelection();
      case SDLK_UP:
        shell_.MoveBufferSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveBufferSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        shell_.MoveBufferSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveBufferSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.buffer_search.query)) {
          shell_.RefreshBufferSearch();
        }
        return true;
      default:
        return false;
    }
  }

  if (shell_.surface_.overlay_mode == OverlayMode::BufferReplace) {
    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.surface_.overlay_visible = false;
        shell_.surface_.focus = FocusTarget::Editor;
        return true;
      case SDLK_TAB:
        shell_.surface_.buffer_search_field =
            shell_.surface_.buffer_search_field == BufferSearchField::Search
                ? BufferSearchField::Replace
                : BufferSearchField::Search;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (modifiers & SDL_KMOD_CTRL) {
          shell_.ReplaceAllBufferSearchMatches();
        } else {
          shell_.ReplaceCurrentBufferSearchMatch();
        }
        return true;
      case SDLK_UP:
        shell_.MoveBufferSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveBufferSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        shell_.MoveBufferSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveBufferSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (shell_.surface_.buffer_search_field == BufferSearchField::Search) {
          if (RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.buffer_search.query)) {
            shell_.RefreshBufferSearch();
          }
        } else {
          RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.buffer_search.replace_text);
        }
        return true;
      default:
        return false;
    }
  }

  if (shell_.surface_.overlay_mode == OverlayMode::ProjectSearch) {
    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.surface_.overlay_visible = false;
        shell_.surface_.focus = FocusTarget::Editor;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return shell_.ActivateOverlaySelection();
      case SDLK_UP:
        shell_.MoveProjectSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveProjectSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        shell_.MoveProjectSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveProjectSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.project_search.query)) {
          shell_.RefreshProjectSearch();
        }
        return true;
      default:
        return false;
    }
  }

  switch (event.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return shell_.ActivateOverlaySelection();
    case SDLK_UP:
      shell_.MoveFileFinderSelection(-1);
      return true;
    case SDLK_DOWN:
      shell_.MoveFileFinderSelection(1);
      return true;
    case SDLK_PAGEUP:
      shell_.MoveFileFinderSelection(-8);
      return true;
    case SDLK_PAGEDOWN:
      shell_.MoveFileFinderSelection(8);
      return true;
    case SDLK_BACKSPACE:
      shell_.file_finder_.Backspace();
      shell_.ResetOverlayScroll();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleSidebarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    const char input_character = shell_.KeycodeToAscii(event.key, modifiers);
    if (shell_.overlay_workflow_.project_search.editing) {
      switch (event.key) {
        case SDLK_ESCAPE:
          shell_.CancelProjectSearchEdit();
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          shell_.CommitProjectSearchEdit();
          return true;
        case SDLK_BACKSPACE:
          RemoveLastUtf8Codepoint(&shell_.overlay_workflow_.project_search.edit_buffer);
          return true;
        default:
          return false;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        if (shell_.surface_.sidebar_temporary) {
          shell_.CloseSidebar();
          return true;
        }
        return false;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        if (!shell_.overlay_workflow_.project_search.results.empty() &&
            shell_.overlay_workflow_.project_search.selected_index <
                shell_.overlay_workflow_.project_search.results.size()) {
          const auto& result =
              shell_.overlay_workflow_.project_search.results[shell_.overlay_workflow_.project_search.selected_index];
          shell_.OpenFile(shell_.project_root_ / result.relative_path);
          shell_.text_viewport_.MoveCursorTo(result.line, result.column);
          if (shell_.surface_.sidebar_temporary) {
            shell_.RestorePreviousSidebar();
          }
          shell_.surface_.focus = FocusTarget::Editor;
        }
        return true;
      case SDLK_UP:
        shell_.MoveProjectSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveProjectSearchSelection(1);
        return true;
      case SDLK_HOME:
        if (!shell_.overlay_workflow_.project_search.results.empty()) {
          shell_.overlay_workflow_.project_search.selected_index = 0;
        }
        return true;
      case SDLK_END:
        if (!shell_.overlay_workflow_.project_search.results.empty()) {
          shell_.overlay_workflow_.project_search.selected_index =
              shell_.overlay_workflow_.project_search.results.size() - 1;
        }
        return true;
      case SDLK_PAGEUP:
        shell_.MoveProjectSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveProjectSearchSelection(8);
        return true;
      case SDLK_R:
        if (input_character == 'R') {
          shell_.ReplaceAllProjectSearchMatches();
        } else {
          shell_.RefreshProjectSearch();
        }
        return true;
      case SDLK_EQUALS:
        shell_.BeginProjectSearchEdit(ProjectSearchEditField::Replace);
        return true;
      case SDLK_SLASH:
        shell_.BeginProjectSearchEdit(ProjectSearchEditField::Query);
        return true;
      default:
        if (event.key == SDLK_J && input_character == 'j') {
          shell_.MoveProjectSearchSelection(1);
          return true;
        }
        if (event.key == SDLK_K && input_character == 'k') {
          shell_.MoveProjectSearchSelection(-1);
          return true;
        }
        return false;
    }
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    switch (event.key) {
      case SDLK_UP:
        shell_.MoveGitSidebarSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveGitSidebarSelection(1);
        return true;
      case SDLK_HOME:
        if (!shell_.git_sidebar_.entries.empty()) {
          shell_.git_sidebar_.selected_index = 0;
          shell_.RevealSelectedGitSidebarLine();
        }
        return true;
      case SDLK_END:
        if (!shell_.git_sidebar_.entries.empty()) {
          shell_.git_sidebar_.selected_index = shell_.git_sidebar_.entries.size() - 1;
          shell_.RevealSelectedGitSidebarLine();
        }
        return true;
      case SDLK_PAGEUP:
        shell_.MoveGitSidebarSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveGitSidebarSelection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return shell_.OpenGitSidebarEntry(shell_.git_sidebar_.selected_index);
      case SDLK_R:
        return ActionCoordinator(shell_).Execute(ActionId::GitRefresh, {}, ActionSource::Shortcut);
      default: {
        const char input_character = shell_.KeycodeToAscii(event.key, modifiers);
        if (input_character == 's') {
          return shell_.StageGitSidebarEntry(shell_.git_sidebar_.selected_index);
        }
        if (input_character == 'u') {
          return shell_.UnstageGitSidebarEntry(shell_.git_sidebar_.selected_index);
        }
        if (input_character == 'x') {
          return shell_.DiscardGitSidebarEntry(shell_.git_sidebar_.selected_index);
        }
        return false;
      }
    }
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    switch (event.key) {
      case SDLK_ESCAPE:
        if (shell_.surface_.sidebar_temporary) {
          shell_.CloseSidebar();
          return true;
        }
        return false;
      case SDLK_UP:
        shell_.MoveProblemsSidebarSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MoveProblemsSidebarSelection(1);
        return true;
      case SDLK_HOME:
        if (!shell_.problems_sidebar_.entries.empty()) {
          shell_.problems_sidebar_.selected_index = 0;
          shell_.RevealSelectedProblemsSidebarLine();
        }
        return true;
      case SDLK_END:
        if (!shell_.problems_sidebar_.entries.empty()) {
          shell_.problems_sidebar_.selected_index = shell_.problems_sidebar_.entries.size() - 1;
          shell_.RevealSelectedProblemsSidebarLine();
        }
        return true;
      case SDLK_PAGEUP:
        shell_.MoveProblemsSidebarSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MoveProblemsSidebarSelection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return shell_.OpenSelectedProblemSidebarItem();
      case SDLK_R:
        return shell_.RefreshProblemsSidebar();
      default:
        return false;
    }
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    switch (event.key) {
      case SDLK_ESCAPE:
        if (shell_.surface_.sidebar_temporary) {
          shell_.CloseSidebar();
          return true;
        }
        return false;
      case SDLK_UP:
        shell_.MovePluginSidebarSelection(-1);
        return true;
      case SDLK_DOWN:
        shell_.MovePluginSidebarSelection(1);
        return true;
      case SDLK_HOME:
        if (!shell_.plugin_sidebar_.items.empty()) {
          shell_.plugin_sidebar_.selected_index = 0;
          shell_.RevealSelectedPluginSidebarLine();
        }
        return true;
      case SDLK_END:
        if (!shell_.plugin_sidebar_.items.empty()) {
          shell_.plugin_sidebar_.selected_index = shell_.plugin_sidebar_.items.size() - 1;
          shell_.RevealSelectedPluginSidebarLine();
        }
        return true;
      case SDLK_PAGEUP:
        shell_.MovePluginSidebarSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        shell_.MovePluginSidebarSelection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return shell_.OpenSelectedPluginSidebarItem();
      case SDLK_R:
        return shell_.RefreshPluginSidebar();
      default:
        return false;
    }
  }

  switch (event.key) {
    case SDLK_UP:
      shell_.directory_tree_.MoveSelection(-1);
      shell_.RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_DOWN:
      shell_.directory_tree_.MoveSelection(1);
      shell_.RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_LEFT:
      shell_.directory_tree_.CollapseSelection();
      shell_.RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_RIGHT:
      shell_.directory_tree_.ExpandSelection();
      shell_.RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const auto opened = shell_.directory_tree_.ActivateSelection();
      shell_.RevealSelectedTreeSidebarLine();
      if (opened.has_value()) {
        shell_.OpenFile(*opened);
      }
      return true;
    }
    case SDLK_R:
      shell_.RefreshProjectFiles();
      return true;
    case SDLK_D:
      shell_.OpenComparePicker();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleCompareKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab != nullptr && compare_tab->right_editable && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      const bool was_dirty = viewport.dirty();
      const std::size_t cursor_before_line = viewport.cursor_line();
      const std::vector<std::string> before_lines = viewport.lines();
      edit();
      shell_.RefreshCompareTabDerivedState(*compare_tab);
      shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport.lines());
      if (viewport.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    };
    const auto sync_compare_navigation = [&](std::size_t previous_selected_row) {
      shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      shell_.ResetCaretBlink();
      if (compare_tab->selected_row != previous_selected_row) {
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(compare_tab->selected_row, compare_tab->selected_row + 1);
      } else {
        shell_.RequestFocusedEditorRedraw();
      }
      return true;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      if (event.key == SDLK_LEFTBRACKET) {
        shell_.JumpCompareHunk(-1);
        return true;
      }
      if (event.key == SDLK_RIGHTBRACKET) {
        shell_.JumpCompareHunk(1);
        return true;
      }
      const char input_character = shell_.KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
      if (input_character == 'o') {
        shell_.OpenWorkingFileFromCompare();
        return true;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.RequestCloseTab(shell_.active_tab_index_);
        return true;
      case SDLK_TAB:
        return apply_compare_edit([&]() { viewport.InsertTab(); });
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return apply_compare_edit([&]() { viewport.InsertNewline(); });
      case SDLK_BACKSPACE:
        return apply_compare_edit([&]() { viewport.Backspace(); });
      case SDLK_DELETE:
        return apply_compare_edit([&]() { viewport.DeleteForward(); });
      case SDLK_UP:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_DOWN:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_LEFT:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_RIGHT:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_PAGEUP:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.Page(-1);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_PAGEDOWN:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.Page(1);
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_HOME:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        if (modifiers & SDL_KMOD_CTRL) {
          viewport.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation(previous_selected_row);
        }
      case SDLK_END:
        {
          const std::size_t previous_selected_row = compare_tab->selected_row;
        if (modifiers & SDL_KMOD_CTRL) {
          const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
          viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation(previous_selected_row);
        }
      default:
        break;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.RequestCloseTab(shell_.active_tab_index_);
      return true;
    case SDLK_UP:
      shell_.MoveCompareSelection(-1);
      return true;
    case SDLK_DOWN:
      shell_.MoveCompareSelection(1);
      return true;
    case SDLK_PAGEUP:
      shell_.MoveCompareSelection(-20);
      return true;
    case SDLK_PAGEDOWN:
      shell_.MoveCompareSelection(20);
      return true;
    case SDLK_HOME:
      if (auto* active_compare_tab = shell_.ActiveCompareTab(); active_compare_tab != nullptr) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = 0;
        shell_.RevealActiveCompareSelection();
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(active_compare_tab->selected_row,
                                     active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_END:
      if (auto* active_compare_tab = shell_.ActiveCompareTab();
          active_compare_tab != nullptr && !active_compare_tab->model.rows.empty()) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = active_compare_tab->model.rows.size() - 1;
        shell_.RevealActiveCompareSelection();
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(active_compare_tab->selected_row,
                                     active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_LEFTBRACKET:
      shell_.JumpCompareHunk(-1);
      return true;
    case SDLK_RIGHTBRACKET:
      shell_.JumpCompareHunk(1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.OpenWorkingFileFromCompare();
      return true;
    default: {
      const char input_character = shell_.KeycodeToAscii(event.key, modifiers);
      if (input_character == 'j') {
        shell_.MoveCompareSelection(1);
        return true;
      }
      if (input_character == 'k') {
        shell_.MoveCompareSelection(-1);
        return true;
      }
      if (input_character == 'o') {
        shell_.OpenWorkingFileFromCompare();
        return true;
      }
      return false;
    }
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleMergeKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  auto& viewport = merge_tab->result_viewport;
  const auto apply_merge_edit = [&](auto&& edit) {
    const bool was_dirty = viewport.dirty();
    const std::size_t cursor_before_line = viewport.cursor_line();
    const std::vector<std::string> before_lines = viewport.lines();
    const std::optional<editor::SelectionRange> selection_before = viewport.selection_range();
    const editor::TextPosition cursor_before{viewport.cursor_line(), viewport.cursor_column()};
    edit();
    shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                         cursor_before);
    shell_.ResetCaretBlink();
    shell_.RequestActiveEditableChangeRedraw(before_lines, viewport.lines());
    if (viewport.dirty() != was_dirty) {
      shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport.cursor_line());
      shell_.RequestTabStripRedraw();
    }
    return true;
  };
  const auto sync_merge_navigation = [&]() {
    merge_tab->scroll_row = static_cast<int>(viewport.scroll_line());
    merge_tab->horizontal_scroll = viewport.horizontal_scroll();
    shell_.ResetCaretBlink();
    shell_.RequestFocusedEditorRedraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const char input_character = shell_.KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
    if (event.key == SDLK_LEFTBRACKET) {
      shell_.MoveMergeSelection(-1);
      return true;
    }
    if (event.key == SDLK_RIGHTBRACKET) {
      shell_.MoveMergeSelection(1);
      return true;
    }
    if (input_character == 'i') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Incoming);
      return true;
    }
    if (input_character == 'c') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Current);
      return true;
    }
    if (input_character == 'b') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Base);
      return true;
    }
    if (input_character == 'm') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Both);
      return true;
    }
    if (input_character == 'o') {
      shell_.OpenMergeResultFile();
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.RequestCloseTab(shell_.active_tab_index_);
      return true;
    case SDLK_TAB:
      return apply_merge_edit([&]() { viewport.InsertTab(); });
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return apply_merge_edit([&]() { viewport.InsertNewline(); });
    case SDLK_BACKSPACE:
      return apply_merge_edit([&]() { viewport.Backspace(); });
    case SDLK_DELETE:
      return apply_merge_edit([&]() { viewport.DeleteForward(); });
    case SDLK_UP:
      viewport.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_DOWN:
      viewport.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_LEFT:
      viewport.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_RIGHT:
      viewport.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_PAGEUP:
      viewport.Page(-1);
      return sync_merge_navigation();
    case SDLK_PAGEDOWN:
      viewport.Page(1);
      return sync_merge_navigation();
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      return sync_merge_navigation();
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
        viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                              (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      return sync_merge_navigation();
    default:
      return false;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event,
                                                SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_TAB:
      {
        const bool was_dirty = shell_.text_viewport_.dirty();
        const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
        const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
        shell_.text_viewport_.InsertTab();
        shell_.ResetCaretBlink();
        shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
        if (shell_.text_viewport_.dirty() != was_dirty) {
          shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                      shell_.text_viewport_.cursor_line());
          shell_.RequestTabStripRedraw();
        }
        return true;
      }
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      {
        const bool was_dirty = shell_.text_viewport_.dirty();
        const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
        const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
        shell_.text_viewport_.InsertNewline();
        shell_.ResetCaretBlink();
        shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
        if (shell_.text_viewport_.dirty() != was_dirty) {
          shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                      shell_.text_viewport_.cursor_line());
          shell_.RequestTabStripRedraw();
        }
        return true;
      }
    case SDLK_BACKSPACE:
      {
        const bool was_dirty = shell_.text_viewport_.dirty();
        const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
        const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
        shell_.text_viewport_.Backspace();
        shell_.ResetCaretBlink();
        shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
        if (shell_.text_viewport_.dirty() != was_dirty) {
          shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                      shell_.text_viewport_.cursor_line());
          shell_.RequestTabStripRedraw();
        }
        return true;
      }
    case SDLK_DELETE:
      {
        const bool was_dirty = shell_.text_viewport_.dirty();
        const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
        const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
        shell_.text_viewport_.DeleteForward();
        shell_.ResetCaretBlink();
        shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
        if (shell_.text_viewport_.dirty() != was_dirty) {
          shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                      shell_.text_viewport_.cursor_line());
          shell_.RequestTabStripRedraw();
        }
        return true;
      }
    default:
      break;
  }

  switch (event.key) {
    case SDLK_UP:
      shell_.text_viewport_.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_DOWN:
      shell_.text_viewport_.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_LEFT:
      shell_.text_viewport_.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_RIGHT:
      shell_.text_viewport_.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_PAGEUP:
      shell_.text_viewport_.Page(-1);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_PAGEDOWN:
      shell_.text_viewport_.Page(1);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        shell_.text_viewport_.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        shell_.text_viewport_.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      shell_.ResetCaretBlink();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line =
            shell_.text_viewport_.line_count() == 0 ? 0 : shell_.text_viewport_.line_count() - 1;
        shell_.text_viewport_.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                    (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        shell_.text_viewport_.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      shell_.ResetCaretBlink();
      return true;
    default:
      return false;
  }
}

}  // namespace microide::workspace
