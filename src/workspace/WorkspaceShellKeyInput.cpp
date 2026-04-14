#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <limits>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

bool WorkspaceShell::HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers) {
  (void) modifiers;
  switch (event.key) {
    case SDLK_ESCAPE:
      prompts_.dirty.selected_action = 2;
      ConfirmDirtyPrompt();
      return true;
    case SDLK_LEFT:
      prompts_.dirty.selected_action = std::max(0, prompts_.dirty.selected_action - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      prompts_.dirty.selected_action = std::min(2, prompts_.dirty.selected_action + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      ConfirmDirtyPrompt();
      return true;
    default: {
      const char input_character = KeycodeToAscii(event.key, SDL_GetModState());
      if (input_character == 's') {
        prompts_.dirty.selected_action = 0;
        ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'd') {
        prompts_.dirty.selected_action = 1;
        ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'c') {
        prompts_.dirty.selected_action = 2;
        ConfirmDirtyPrompt();
        return true;
      }
      return true;
    }
  }
}

bool WorkspaceShell::HandleTreeContextMenuKeyDown(const SDL_KeyboardEvent& event) {
  switch (event.key) {
    case SDLK_ESCAPE:
      CloseTreeContextMenu();
      return true;
    case SDLK_DOWN:
      surface_.tree_context_menu.active_item_index =
          NextEnabledTreeContextMenuItemIndex(surface_.tree_context_menu.active_item_index, 1);
      return true;
    case SDLK_UP:
      surface_.tree_context_menu.active_item_index =
          NextEnabledTreeContextMenuItemIndex(surface_.tree_context_menu.active_item_index, -1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (surface_.tree_context_menu.active_item_index >= 0) {
        return ExecuteTreeContextMenuItem(
            static_cast<std::size_t>(surface_.tree_context_menu.active_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::HandleMenuBarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_ESCAPE:
      CloseMenuBar();
      return true;
    case SDLK_LEFT:
      return SwitchMenuBarMenu(-1);
    case SDLK_RIGHT:
      return SwitchMenuBarMenu(1);
    case SDLK_TAB:
      return SwitchMenuBarMenu((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    case SDLK_DOWN:
      return MoveActiveMenuItem(1);
    case SDLK_UP:
      return MoveActiveMenuItem(-1);
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (surface_.active_menu_item_index >= 0) {
        return ExecuteMenuItem(surface_.active_menu_id,
                               static_cast<std::size_t>(surface_.active_menu_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event) {
  if (prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    switch (event.key) {
      case SDLK_ESCAPE:
        DismissPromptSurface(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        prompts_.surface.selected_button = 0;
        ConfirmPromptSurface();
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&prompts_.surface.input);
        return true;
      default:
        return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      DismissPromptSurface(true);
      return true;
    case SDLK_LEFT:
      prompts_.surface.selected_button = std::max(0, prompts_.surface.selected_button - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      prompts_.surface.selected_button = std::min(1, prompts_.surface.selected_button + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      ConfirmPromptSurface();
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                         SDL_Keymod modifiers,
                                         bool active_compare_tab,
                                         bool active_merge_tab) {
  if ((modifiers & SDL_KMOD_CTRL) && !surface_.command_mode && !surface_.overlay_visible &&
      event.key == SDLK_N) {
    return OpenUntitledTab();
  }

  if ((modifiers & SDL_KMOD_CTRL) && !surface_.command_mode && !surface_.overlay_visible &&
      surface_.focus == FocusTarget::Editor && !active_compare_tab &&
      ActiveEditableViewport() != nullptr && event.key == SDLK_A) {
    ExecuteAction(ActionId::SelectAll, {}, ActionSource::Shortcut);
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && !surface_.command_mode && !surface_.overlay_visible &&
      surface_.focus == FocusTarget::Editor && !active_compare_tab) {
    if (!active_merge_tab && (modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_F) {
      ExecuteAction(ActionId::ProjectSearch, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key == SDLK_H) {
      ExecuteAction(ActionId::ReplaceInBuffer, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key == SDLK_F) {
      ExecuteAction(ActionId::Search, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_W) {
      ExecuteAction(ActionId::CloseActiveTab, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_Z) {
      ExecuteAction((modifiers & SDL_KMOD_SHIFT) != 0 ? ActionId::Redo : ActionId::Undo, {},
                    ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_Y) {
      ExecuteAction(ActionId::Redo, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_C) {
      ExecuteAction(ActionId::CopySelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_X) {
      ExecuteAction(ActionId::CutSelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_V) {
      ExecuteAction(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && !active_compare_tab && event.key == SDLK_S) {
    ExecuteAction(ActionId::Save, {}, ActionSource::Shortcut);
    return true;
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key == SDLK_0 || event.key == SDLK_KP_0) {
      ExecuteAction(ActionId::UiScale, {"reset"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_MINUS || event.key == SDLK_KP_MINUS) {
      ExecuteAction(ActionId::UiScale, {"down"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key == SDLK_EQUALS || event.key == SDLK_PLUS || event.key == SDLK_KP_PLUS) {
      ExecuteAction(ActionId::UiScale, {"up"}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_E) {
    ExecuteAction(ActionId::OpenCommandPrompt, {}, ActionSource::Shortcut);
    return true;
  }

  return false;
}

bool WorkspaceShell::HandleCommandKeyDown(const SDL_KeyboardEvent& event) {
  switch (event.key) {
    case SDLK_ESCAPE:
      surface_.command_mode = false;
      command_.input.clear();
      ResetCommandSessionState();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (command_.input.empty() || ExecuteCommand(command_.input)) {
        surface_.command_mode = false;
        command_.input.clear();
        ResetCommandSessionState();
      }
      return true;
    case SDLK_BACKSPACE:
      RemoveLastUtf8Codepoint(&command_.input);
      command_.history_index.reset();
      command_.history_pending_input.clear();
      ClearCommandFeedback();
      return true;
    case SDLK_UP:
      StepCommandHistory(-1);
      return true;
    case SDLK_DOWN:
      StepCommandHistory(1);
      return true;
    case SDLK_TAB:
      CompleteCommandInput();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event,
                                                    SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_F8:
      ExecuteAction(ActionId::SidebarToggle, {}, ActionSource::Shortcut);
      return true;
    case SDLK_F6:
      ExecuteAction(ActionId::Files, {}, ActionSource::Shortcut);
      return true;
    case SDLK_TAB:
      if (modifiers & SDL_KMOD_CTRL) {
        if (surface_.overlay_visible) {
          surface_.focus = FocusTarget::Overlay;
          return true;
        }
        const bool include_panel = BottomPanelVisible();
        if (include_panel) {
          if (surface_.sidebar_visible) {
            if (modifiers & SDL_KMOD_SHIFT) {
              surface_.focus = surface_.focus == FocusTarget::Sidebar
                                   ? FocusTarget::Panel
                                   : surface_.focus == FocusTarget::Panel
                                         ? FocusTarget::Editor
                                         : FocusTarget::Sidebar;
            } else {
              surface_.focus = surface_.focus == FocusTarget::Sidebar
                                   ? FocusTarget::Editor
                                   : surface_.focus == FocusTarget::Editor
                                         ? FocusTarget::Panel
                                         : FocusTarget::Sidebar;
            }
          } else {
            surface_.focus =
                surface_.focus == FocusTarget::Panel ? FocusTarget::Editor : FocusTarget::Panel;
          }
        } else if (surface_.sidebar_visible && !(modifiers & SDL_KMOD_SHIFT)) {
          surface_.focus =
              surface_.focus == FocusTarget::Sidebar ? FocusTarget::Editor : FocusTarget::Sidebar;
        } else if (surface_.sidebar_visible) {
          surface_.focus =
              surface_.focus == FocusTarget::Editor ? FocusTarget::Sidebar : FocusTarget::Editor;
        } else {
          surface_.focus = FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (surface_.overlay_visible) {
        DismissOverlay();
        return true;
      }
      if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible &&
          surface_.sidebar_temporary && surface_.sidebar_mode == SidebarMode::Search) {
        CloseSidebar();
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

bool WorkspaceShell::HandleOverlayKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  if (surface_.overlay_mode == OverlayMode::CommitPicker) {
    switch (event.key) {
      case SDLK_ESCAPE:
        DismissOverlay();
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return ActivateOverlaySelection();
      case SDLK_UP:
        MoveComparePickerSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveComparePickerSelection(1);
        return true;
      case SDLK_HOME:
        if (!overlay_workflow_.compare_picker.matches.empty()) {
          overlay_workflow_.compare_picker.selected_index = 0;
          if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
            RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
          }
        }
        return true;
      case SDLK_END:
        if (!overlay_workflow_.compare_picker.matches.empty()) {
          overlay_workflow_.compare_picker.selected_index =
              overlay_workflow_.compare_picker.matches.size() - 1;
          if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
            RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
          }
        }
        return true;
      case SDLK_PAGEUP:
        MoveComparePickerSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveComparePickerSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&overlay_workflow_.compare_picker.query)) {
          RefreshComparePicker();
        }
        return true;
      default:
        return false;
    }
  }

  if (surface_.overlay_mode == OverlayMode::BufferSearch) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return ActivateOverlaySelection();
      case SDLK_UP:
        MoveBufferSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveBufferSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        MoveBufferSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveBufferSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&overlay_workflow_.buffer_search.query)) {
          RefreshBufferSearch();
        }
        return true;
      default:
        return false;
    }
  }

  if (surface_.overlay_mode == OverlayMode::BufferReplace) {
    switch (event.key) {
      case SDLK_ESCAPE:
        surface_.overlay_visible = false;
        surface_.focus = FocusTarget::Editor;
        return true;
      case SDLK_TAB:
        surface_.buffer_search_field =
            surface_.buffer_search_field == BufferSearchField::Search
                ? BufferSearchField::Replace
                : BufferSearchField::Search;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (modifiers & SDL_KMOD_CTRL) {
          ReplaceAllBufferSearchMatches();
        } else {
          ReplaceCurrentBufferSearchMatch();
        }
        return true;
      case SDLK_UP:
        MoveBufferSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveBufferSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        MoveBufferSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveBufferSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (surface_.buffer_search_field == BufferSearchField::Search) {
          if (RemoveLastUtf8Codepoint(&overlay_workflow_.buffer_search.query)) {
            RefreshBufferSearch();
          }
        } else {
          RemoveLastUtf8Codepoint(&overlay_workflow_.buffer_search.replace_text);
        }
        return true;
      default:
        return false;
    }
  }

  if (surface_.overlay_mode == OverlayMode::ProjectSearch) {
    switch (event.key) {
      case SDLK_ESCAPE:
        surface_.overlay_visible = false;
        surface_.focus = FocusTarget::Editor;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return ActivateOverlaySelection();
      case SDLK_UP:
        MoveProjectSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveProjectSearchSelection(1);
        return true;
      case SDLK_PAGEUP:
        MoveProjectSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveProjectSearchSelection(8);
        return true;
      case SDLK_BACKSPACE:
        if (RemoveLastUtf8Codepoint(&overlay_workflow_.project_search.query)) {
          RefreshProjectSearch();
        }
        return true;
      default:
        return false;
    }
  }

  switch (event.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return ActivateOverlaySelection();
    case SDLK_UP:
      MoveFileFinderSelection(-1);
      return true;
    case SDLK_DOWN:
      MoveFileFinderSelection(1);
      return true;
    case SDLK_PAGEUP:
      MoveFileFinderSelection(-8);
      return true;
    case SDLK_PAGEDOWN:
      MoveFileFinderSelection(8);
      return true;
    case SDLK_BACKSPACE:
      file_finder_.Backspace();
      ResetOverlayScroll();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::HandleSidebarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  if (surface_.sidebar_mode == SidebarMode::Search) {
    const char input_character = KeycodeToAscii(event.key, modifiers);
    if (overlay_workflow_.project_search.editing) {
      switch (event.key) {
        case SDLK_ESCAPE:
          CancelProjectSearchEdit();
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          CommitProjectSearchEdit();
          return true;
        case SDLK_BACKSPACE:
          RemoveLastUtf8Codepoint(&overlay_workflow_.project_search.edit_buffer);
          return true;
        default:
          return false;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        if (surface_.sidebar_temporary) {
          CloseSidebar();
          return true;
        }
        return false;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        if (!overlay_workflow_.project_search.results.empty() &&
            overlay_workflow_.project_search.selected_index <
                overlay_workflow_.project_search.results.size()) {
          const auto& result =
              overlay_workflow_.project_search.results[overlay_workflow_.project_search.selected_index];
          OpenFile(project_root_ / result.relative_path);
          text_viewport_.MoveCursorTo(result.line, result.column);
          if (surface_.sidebar_temporary) {
            RestorePreviousSidebar();
          }
          surface_.focus = FocusTarget::Editor;
        }
        return true;
      case SDLK_UP:
        MoveProjectSearchSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveProjectSearchSelection(1);
        return true;
      case SDLK_HOME:
        if (!overlay_workflow_.project_search.results.empty()) {
          overlay_workflow_.project_search.selected_index = 0;
        }
        return true;
      case SDLK_END:
        if (!overlay_workflow_.project_search.results.empty()) {
          overlay_workflow_.project_search.selected_index =
              overlay_workflow_.project_search.results.size() - 1;
        }
        return true;
      case SDLK_PAGEUP:
        MoveProjectSearchSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveProjectSearchSelection(8);
        return true;
      case SDLK_R:
        if (input_character == 'R') {
          ReplaceAllProjectSearchMatches();
        } else {
          RefreshProjectSearch();
        }
        return true;
      case SDLK_EQUALS:
        BeginProjectSearchEdit(ProjectSearchEditField::Replace);
        return true;
      case SDLK_SLASH:
        BeginProjectSearchEdit(ProjectSearchEditField::Query);
        return true;
      default:
        if (event.key == SDLK_J && input_character == 'j') {
          MoveProjectSearchSelection(1);
          return true;
        }
        if (event.key == SDLK_K && input_character == 'k') {
          MoveProjectSearchSelection(-1);
          return true;
        }
        return false;
    }
  }

  if (surface_.sidebar_mode == SidebarMode::Git) {
    switch (event.key) {
      case SDLK_UP:
        MoveGitSidebarSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveGitSidebarSelection(1);
        return true;
      case SDLK_HOME:
        if (!git_sidebar_.entries.empty()) {
          git_sidebar_.selected_index = 0;
          RevealSelectedGitSidebarLine();
        }
        return true;
      case SDLK_END:
        if (!git_sidebar_.entries.empty()) {
          git_sidebar_.selected_index = git_sidebar_.entries.size() - 1;
          RevealSelectedGitSidebarLine();
        }
        return true;
      case SDLK_PAGEUP:
        MoveGitSidebarSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveGitSidebarSelection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return OpenGitSidebarEntry(git_sidebar_.selected_index);
      case SDLK_R:
        return ExecuteAction(ActionId::GitRefresh, {}, ActionSource::Shortcut);
      default: {
        const char input_character = KeycodeToAscii(event.key, modifiers);
        if (input_character == 's') {
          return StageGitSidebarEntry(git_sidebar_.selected_index);
        }
        if (input_character == 'u') {
          return UnstageGitSidebarEntry(git_sidebar_.selected_index);
        }
        if (input_character == 'x') {
          return DiscardGitSidebarEntry(git_sidebar_.selected_index);
        }
        return false;
      }
    }
  }

  switch (event.key) {
    case SDLK_UP:
      directory_tree_.MoveSelection(-1);
      RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_DOWN:
      directory_tree_.MoveSelection(1);
      RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_LEFT:
      directory_tree_.CollapseSelection();
      RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_RIGHT:
      directory_tree_.ExpandSelection();
      RevealSelectedTreeSidebarLine();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const auto opened = directory_tree_.ActivateSelection();
      RevealSelectedTreeSidebarLine();
      if (opened.has_value()) {
        OpenFile(*opened);
      }
      return true;
    }
    case SDLK_R:
      RefreshProjectFiles();
      return true;
    case SDLK_D:
      OpenComparePicker();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::HandleCompareKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr && compare_tab->right_editable && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      edit();
      RefreshCompareTabDerivedState(*compare_tab);
      SyncCompareSelectionFromViewport(*compare_tab, true);
      ResetCaretBlink();
      return true;
    };
    const auto sync_compare_navigation = [&]() {
      SyncCompareSelectionFromViewport(*compare_tab, true);
      ResetCaretBlink();
      return true;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      if (event.key == SDLK_LEFTBRACKET) {
        JumpCompareHunk(-1);
        return true;
      }
      if (event.key == SDLK_RIGHTBRACKET) {
        JumpCompareHunk(1);
        return true;
      }
      const char input_character = KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
      if (input_character == 'o') {
        OpenWorkingFileFromCompare();
        return true;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        RequestCloseTab(active_tab_index_);
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
        viewport.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation();
      case SDLK_DOWN:
        viewport.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation();
      case SDLK_LEFT:
        viewport.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation();
      case SDLK_RIGHT:
        viewport.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation();
      case SDLK_PAGEUP:
        viewport.Page(-1);
        return sync_compare_navigation();
      case SDLK_PAGEDOWN:
        viewport.Page(1);
        return sync_compare_navigation();
      case SDLK_HOME:
        if (modifiers & SDL_KMOD_CTRL) {
          viewport.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation();
      case SDLK_END:
        if (modifiers & SDL_KMOD_CTRL) {
          const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
          viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation();
      default:
        break;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      RequestCloseTab(active_tab_index_);
      return true;
    case SDLK_UP:
      MoveCompareSelection(-1);
      return true;
    case SDLK_DOWN:
      MoveCompareSelection(1);
      return true;
    case SDLK_PAGEUP:
      MoveCompareSelection(-20);
      return true;
    case SDLK_PAGEDOWN:
      MoveCompareSelection(20);
      return true;
    case SDLK_HOME:
      if (auto* active_compare_tab = ActiveCompareTab(); active_compare_tab != nullptr) {
        active_compare_tab->selected_row = 0;
        RevealActiveCompareSelection();
      }
      return true;
    case SDLK_END:
      if (auto* active_compare_tab = ActiveCompareTab();
          active_compare_tab != nullptr && !active_compare_tab->model.rows.empty()) {
        active_compare_tab->selected_row = active_compare_tab->model.rows.size() - 1;
        RevealActiveCompareSelection();
      }
      return true;
    case SDLK_LEFTBRACKET:
      JumpCompareHunk(-1);
      return true;
    case SDLK_RIGHTBRACKET:
      JumpCompareHunk(1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      OpenWorkingFileFromCompare();
      return true;
    default: {
      const char input_character = KeycodeToAscii(event.key, modifiers);
      if (input_character == 'j') {
        MoveCompareSelection(1);
        return true;
      }
      if (input_character == 'k') {
        MoveCompareSelection(-1);
        return true;
      }
      if (input_character == 'o') {
        OpenWorkingFileFromCompare();
        return true;
      }
      return false;
    }
  }
}

bool WorkspaceShell::HandleMergeKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  auto& viewport = merge_tab->result_viewport;
  const auto apply_merge_edit = [&](auto&& edit) {
    const std::vector<std::string> before_lines = viewport.lines();
    const std::optional<editor::SelectionRange> selection_before = viewport.selection_range();
    const editor::TextPosition cursor_before{viewport.cursor_line(), viewport.cursor_column()};
    edit();
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                         cursor_before);
    ResetCaretBlink();
    return true;
  };
  const auto sync_merge_navigation = [&]() {
    merge_tab->scroll_row = static_cast<int>(viewport.scroll_line());
    merge_tab->horizontal_scroll = viewport.horizontal_scroll();
    ResetCaretBlink();
    return true;
  };

  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const char input_character = KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
    if (event.key == SDLK_LEFTBRACKET) {
      MoveMergeSelection(-1);
      return true;
    }
    if (event.key == SDLK_RIGHTBRACKET) {
      MoveMergeSelection(1);
      return true;
    }
    if (input_character == 'i') {
      ApplyMergeChoice(compare::MergeChoice::Incoming);
      return true;
    }
    if (input_character == 'c') {
      ApplyMergeChoice(compare::MergeChoice::Current);
      return true;
    }
    if (input_character == 'b') {
      ApplyMergeChoice(compare::MergeChoice::Base);
      return true;
    }
    if (input_character == 'm') {
      ApplyMergeChoice(compare::MergeChoice::Both);
      return true;
    }
    if (input_character == 'o') {
      OpenMergeResultFile();
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      RequestCloseTab(active_tab_index_);
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

bool WorkspaceShell::HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event,
                                                SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_TAB:
      text_viewport_.InsertTab();
      ResetCaretBlink();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      text_viewport_.InsertNewline();
      ResetCaretBlink();
      return true;
    case SDLK_BACKSPACE:
      text_viewport_.Backspace();
      ResetCaretBlink();
      return true;
    case SDLK_DELETE:
      text_viewport_.DeleteForward();
      ResetCaretBlink();
      return true;
    default:
      break;
  }

  switch (event.key) {
    case SDLK_UP:
      text_viewport_.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_DOWN:
      text_viewport_.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_LEFT:
      text_viewport_.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_RIGHT:
      text_viewport_.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_PAGEUP:
      text_viewport_.Page(-1);
      ResetCaretBlink();
      return true;
    case SDLK_PAGEDOWN:
      text_viewport_.Page(1);
      ResetCaretBlink();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        text_viewport_.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        text_viewport_.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      ResetCaretBlink();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line =
            text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1;
        text_viewport_.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                    (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        text_viewport_.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      ResetCaretBlink();
      return true;
    default:
      return false;
  }
}

}  // namespace microide::workspace
