#include "workspace/WorkspaceShell.h"

#include <limits>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

struct ScopeExit {
  std::function<void()> on_exit;

  ~ScopeExit() {
    if (on_exit) {
      on_exit();
    }
  }
};

}  // namespace

bool WorkspaceShell::HandleEvent(const SDL_Event& event) {
  const ScopeExit sync_terminal_focus{[this]() { SyncTerminalFocusState(); }};

  if (project_open_dialog_event_type_ != 0 && event.type == project_open_dialog_event_type_) {
    ConsumePendingProjectOpenDialogResult();
    return true;
  }
  if (project_search_event_type_ != 0 && event.type == project_search_event_type_) {
    ConsumeProjectSearchUpdates();
    return true;
  }
  if (git_blame_event_type_ != 0 && event.type == git_blame_event_type_) {
    return true;
  }
  if (terminal_event_type_ != 0 && event.type == terminal_event_type_) {
    ConsumeTerminalSessionUpdates();
    return true;
  }

  SyncTextInputSurface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return HandleMouseButtonDown(event);
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return HandleMouseButtonUp(event);
    case SDL_EVENT_MOUSE_MOTION:
      return HandleMouseMotion(event);
    case SDL_EVENT_MOUSE_WHEEL:
      return HandleMouseWheel(event);
    case SDL_EVENT_TEXT_EDITING:
      return HandleTextEditing(event.edit);
    case SDL_EVENT_TEXT_INPUT:
      return HandleTextInput(event.text);
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      window_has_input_focus_ = true;
      return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      window_has_input_focus_ = false;
      return true;
    case SDL_EVENT_KEY_DOWN:
      break;
    default:
      return false;
  }

  if (dirty_prompt_visible_) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        dirty_prompt_state_.selected_action = 2;
        ConfirmDirtyPrompt();
        return true;
      case SDLK_LEFT:
        dirty_prompt_state_.selected_action =
            std::max(0, dirty_prompt_state_.selected_action - 1);
        return true;
      case SDLK_RIGHT:
      case SDLK_TAB:
        dirty_prompt_state_.selected_action =
            std::min(2, dirty_prompt_state_.selected_action + 1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        ConfirmDirtyPrompt();
        return true;
      default: {
        const char input_character = KeycodeToAscii(event.key.key, SDL_GetModState());
        if (input_character == 's') {
          dirty_prompt_state_.selected_action = 0;
          ConfirmDirtyPrompt();
          return true;
        }
        if (input_character == 'd') {
          dirty_prompt_state_.selected_action = 1;
          ConfirmDirtyPrompt();
          return true;
        }
        if (input_character == 'c') {
          dirty_prompt_state_.selected_action = 2;
          ConfirmDirtyPrompt();
          return true;
        }
        return true;
      }
    }
  }

  const SDL_Keymod modifiers =
      event.key.mod != SDL_KMOD_NONE ? event.key.mod : SDL_GetModState();
  if (tree_context_menu_.open) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        CloseTreeContextMenu();
        return true;
      case SDLK_DOWN:
        tree_context_menu_.active_item_index =
            NextEnabledTreeContextMenuItemIndex(tree_context_menu_.active_item_index, 1);
        return true;
      case SDLK_UP:
        tree_context_menu_.active_item_index =
            NextEnabledTreeContextMenuItemIndex(tree_context_menu_.active_item_index, -1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (tree_context_menu_.active_item_index >= 0) {
          return ExecuteTreeContextMenuItem(
              static_cast<std::size_t>(tree_context_menu_.active_item_index));
        }
        return true;
      default:
        return true;
    }
  }
  if (menu_bar_open_) {
    switch (event.key.key) {
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
        if (active_menu_item_index_ >= 0) {
          return ExecuteMenuItem(active_menu_id_,
                                 static_cast<std::size_t>(active_menu_item_index_));
        }
        return true;
      default:
        return true;
    }
  }
  if (CompositionConsumesKey(event.key.key, modifiers)) {
    return true;
  }
  if (prompt_surface_visible_) {
    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
      switch (event.key.key) {
        case SDLK_ESCAPE: {
          DismissPromptSurface(true);
          return true;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          prompt_surface_state_.selected_button = 0;
          ConfirmPromptSurface();
          return true;
        case SDLK_BACKSPACE:
          RemoveLastUtf8Codepoint(&prompt_surface_state_.input);
          return true;
        default:
          return true;
      }
    }

    switch (event.key.key) {
      case SDLK_ESCAPE: {
        DismissPromptSurface(true);
        return true;
      }
      case SDLK_LEFT:
        prompt_surface_state_.selected_button =
            std::max(0, prompt_surface_state_.selected_button - 1);
        return true;
      case SDLK_RIGHT:
      case SDLK_TAB:
        prompt_surface_state_.selected_button =
            std::min(1, prompt_surface_state_.selected_button + 1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        ConfirmPromptSurface();
        return true;
      default:
        return true;
    }
  }
  const bool active_compare_tab = ActiveTabIsCompare();
  const bool active_merge_tab = ActiveTabIsMerge();
  if ((modifiers & SDL_KMOD_CTRL) && !command_mode_ && !overlay_visible_ &&
      focus_ == FocusTarget::Editor && !active_compare_tab && ActiveEditableViewport() != nullptr &&
      event.key.key == SDLK_A) {
    ExecuteAction(ActionId::SelectAll, {}, ActionSource::Shortcut);
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && !command_mode_ && !overlay_visible_ &&
      focus_ == FocusTarget::Editor && !active_compare_tab) {
    if (!active_merge_tab && (modifiers & SDL_KMOD_SHIFT) && event.key.key == SDLK_F) {
      ExecuteAction(ActionId::ProjectSearch, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key.key == SDLK_H) {
      ExecuteAction(ActionId::ReplaceInBuffer, {}, ActionSource::Shortcut);
      return true;
    }
    if (!active_merge_tab && event.key.key == SDLK_F) {
      ExecuteAction(ActionId::Search, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_W) {
      ExecuteAction(ActionId::CloseActiveTab, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_Z) {
      ExecuteAction((modifiers & SDL_KMOD_SHIFT) != 0 ? ActionId::Redo : ActionId::Undo, {},
                    ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_Y) {
      ExecuteAction(ActionId::Redo, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_C) {
      ExecuteAction(ActionId::CopySelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_X) {
      ExecuteAction(ActionId::CutSelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_V) {
      ExecuteAction(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && !active_compare_tab && event.key.key == SDLK_S) {
    ExecuteAction(ActionId::Save, {}, ActionSource::Shortcut);
    return true;
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key.key == SDLK_0 || event.key.key == SDLK_KP_0) {
      ExecuteAction(ActionId::UiScale, {"reset"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_MINUS || event.key.key == SDLK_KP_MINUS) {
      ExecuteAction(ActionId::UiScale, {"down"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_EQUALS || event.key.key == SDLK_PLUS ||
        event.key.key == SDLK_KP_PLUS) {
      ExecuteAction(ActionId::UiScale, {"up"}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key.key == SDLK_E) {
    ExecuteAction(ActionId::OpenCommandPrompt, {}, ActionSource::Shortcut);
    return true;
  }

  if (command_mode_) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        command_mode_ = false;
        command_input_.clear();
        ResetCommandSessionState();
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (command_input_.empty() || ExecuteCommand(command_input_)) {
          command_mode_ = false;
          command_input_.clear();
          ResetCommandSessionState();
        }
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&command_input_);
        command_history_index_.reset();
        command_history_pending_input_.clear();
        ClearCommandCompletionFeedback();
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

  switch (event.key.key) {
    case SDLK_F8:
      ExecuteAction(ActionId::SidebarToggle, {}, ActionSource::Shortcut);
      return true;
    case SDLK_F6:
      ExecuteAction(ActionId::Files, {}, ActionSource::Shortcut);
      return true;
    case SDLK_TAB:
      if (modifiers & SDL_KMOD_CTRL) {
        if (overlay_visible_) {
          focus_ = FocusTarget::Overlay;
          return true;
        }
        const bool include_panel = BottomPanelVisible();
        if (include_panel) {
          if (sidebar_visible_) {
            if (modifiers & SDL_KMOD_SHIFT) {
              focus_ = focus_ == FocusTarget::Sidebar
                           ? FocusTarget::Panel
                           : focus_ == FocusTarget::Panel ? FocusTarget::Editor
                                                          : FocusTarget::Sidebar;
            } else {
              focus_ = focus_ == FocusTarget::Sidebar
                           ? FocusTarget::Editor
                           : focus_ == FocusTarget::Editor ? FocusTarget::Panel
                                                           : FocusTarget::Sidebar;
            }
          } else {
            focus_ = focus_ == FocusTarget::Panel ? FocusTarget::Editor : FocusTarget::Panel;
          }
        } else if (sidebar_visible_ && !(modifiers & SDL_KMOD_SHIFT)) {
          focus_ = focus_ == FocusTarget::Sidebar ? FocusTarget::Editor : FocusTarget::Sidebar;
        } else if (sidebar_visible_) {
          focus_ = focus_ == FocusTarget::Editor ? FocusTarget::Sidebar : FocusTarget::Editor;
        } else {
          focus_ = FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (overlay_visible_) {
        overlay_visible_ = false;
        focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
        return true;
      }
      if (focus_ == FocusTarget::Sidebar && sidebar_visible_ &&
          sidebar_temporary_ && sidebar_mode_ == SidebarMode::Search) {
        CloseSidebar();
        return true;
      }
      break;
    default:
      break;
  }

  if (focus_ == FocusTarget::Overlay) {
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
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
          if (!compare_picker_matches_.empty()) {
            compare_picker_selected_index_ = 0;
            if (last_window_width_ > 0 && last_window_height_ > 0) {
              const WorkspaceLayout layout = ComputeLayout(
                  static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                  sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
              RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
            }
          }
          return true;
        case SDLK_END:
          if (!compare_picker_matches_.empty()) {
            compare_picker_selected_index_ = compare_picker_matches_.size() - 1;
            if (last_window_width_ > 0 && last_window_height_ > 0) {
              const WorkspaceLayout layout = ComputeLayout(
                  static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                  sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
              RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
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
          if (RemoveLastUtf8Codepoint(&compare_picker_query_)) {
            RefreshComparePicker();
          }
          return true;
        default:
          return false;
      }
    }
    if (overlay_mode_ == OverlayMode::BufferSearch) {
      switch (event.key.key) {
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
          if (RemoveLastUtf8Codepoint(&buffer_search_query_)) {
            RefreshBufferSearch();
          }
          return true;
        default:
          return false;
      }
    }

    if (overlay_mode_ == OverlayMode::BufferReplace) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = FocusTarget::Editor;
          return true;
        case SDLK_TAB:
          buffer_search_field_ = buffer_search_field_ == BufferSearchField::Search
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
          if (buffer_search_field_ == BufferSearchField::Search) {
            if (RemoveLastUtf8Codepoint(&buffer_search_query_)) {
              RefreshBufferSearch();
            }
          } else {
            RemoveLastUtf8Codepoint(&buffer_replace_text_);
          }
          return true;
        default:
          return false;
      }
    }

    if (overlay_mode_ == OverlayMode::ProjectSearch) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = FocusTarget::Editor;
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
          if (RemoveLastUtf8Codepoint(&project_search_query_)) {
            RefreshProjectSearch();
          }
          return true;
        default:
          return false;
      }
    }

    switch (event.key.key) {
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

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_) {
    if (sidebar_mode_ == SidebarMode::Search) {
      const char input_character = KeycodeToAscii(event.key.key, modifiers);
      if (project_search_editing_) {
        switch (event.key.key) {
          case SDLK_ESCAPE:
            CancelProjectSearchEdit();
            return true;
          case SDLK_RETURN:
          case SDLK_KP_ENTER:
            CommitProjectSearchEdit();
            return true;
          case SDLK_BACKSPACE:
            RemoveLastUtf8Codepoint(&project_search_edit_buffer_);
            return true;
          default:
            return false;
        }
      }

      switch (event.key.key) {
        case SDLK_ESCAPE:
          if (sidebar_temporary_) {
            CloseSidebar();
            return true;
          }
          return false;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_RIGHT:
          if (!project_search_results_.empty() &&
              project_search_selected_index_ < project_search_results_.size()) {
            const auto& result = project_search_results_[project_search_selected_index_];
            OpenFile(project_root_ / result.relative_path);
            text_viewport_.MoveCursorTo(result.line, result.column);
            if (sidebar_temporary_) {
              RestorePreviousSidebar();
            }
            focus_ = FocusTarget::Editor;
          }
          return true;
        case SDLK_UP:
          MoveProjectSearchSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveProjectSearchSelection(1);
          return true;
        case SDLK_HOME:
          if (!project_search_results_.empty()) {
            project_search_selected_index_ = 0;
          }
          return true;
        case SDLK_END:
          if (!project_search_results_.empty()) {
            project_search_selected_index_ = project_search_results_.size() - 1;
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
          if (event.key.key == SDLK_J && input_character == 'j') {
            MoveProjectSearchSelection(1);
            return true;
          }
          if (event.key.key == SDLK_K && input_character == 'k') {
            MoveProjectSearchSelection(-1);
            return true;
          }
          return false;
      }
    }

    if (sidebar_mode_ == SidebarMode::Git) {
      switch (event.key.key) {
        case SDLK_UP:
          MoveGitSidebarSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveGitSidebarSelection(1);
          return true;
        case SDLK_HOME:
          if (!git_sidebar_entries_.empty()) {
            git_sidebar_selected_index_ = 0;
            RevealSelectedGitSidebarLine();
          }
          return true;
        case SDLK_END:
          if (!git_sidebar_entries_.empty()) {
            git_sidebar_selected_index_ = git_sidebar_entries_.size() - 1;
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
          return OpenGitSidebarEntry(git_sidebar_selected_index_);
        case SDLK_R:
          return ExecuteAction(ActionId::GitRefresh, {}, ActionSource::Shortcut);
        default: {
          const char input_character = KeycodeToAscii(event.key.key, modifiers);
          if (input_character == 's') {
            return StageGitSidebarEntry(git_sidebar_selected_index_);
          }
          if (input_character == 'u') {
            return UnstageGitSidebarEntry(git_sidebar_selected_index_);
          }
          if (input_character == 'x') {
            return DiscardGitSidebarEntry(git_sidebar_selected_index_);
          }
          return false;
        }
      }
    }

    switch (event.key.key) {
      case SDLK_UP:
        directory_tree_.MoveSelection(-1);
        return true;
      case SDLK_DOWN:
        directory_tree_.MoveSelection(1);
        return true;
      case SDLK_LEFT:
        directory_tree_.CollapseSelection();
        return true;
      case SDLK_RIGHT:
        directory_tree_.ExpandSelection();
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: {
        const auto opened = directory_tree_.ActivateSelection();
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

  if (focus_ == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    return HandleTerminalKeyDown(event.key, modifiers);
  }

  if (focus_ == FocusTarget::Editor && active_compare_tab) {
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
        if (event.key.key == SDLK_LEFTBRACKET) {
          JumpCompareHunk(-1);
          return true;
        }
        if (event.key.key == SDLK_RIGHTBRACKET) {
          JumpCompareHunk(1);
          return true;
        }
        const char input_character = KeycodeToAscii(event.key.key, modifiers & ~SDL_KMOD_ALT);
        if (input_character == 'o') {
          OpenWorkingFileFromCompare();
          return true;
        }
      }

      switch (event.key.key) {
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

    switch (event.key.key) {
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
        if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr) {
          compare_tab->selected_row = 0;
          RevealActiveCompareSelection();
        }
        return true;
      case SDLK_END:
        if (auto* compare_tab = ActiveCompareTab();
            compare_tab != nullptr && !compare_tab->model.rows.empty()) {
          compare_tab->selected_row = compare_tab->model.rows.size() - 1;
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
        const char input_character = KeycodeToAscii(event.key.key, modifiers);
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

  if (focus_ == FocusTarget::Editor && active_merge_tab) {
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
      UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, cursor_before);
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
      const char input_character = KeycodeToAscii(event.key.key, modifiers & ~SDL_KMOD_ALT);
      if (event.key.key == SDLK_LEFTBRACKET) {
        MoveMergeSelection(-1);
        return true;
      }
      if (event.key.key == SDLK_RIGHTBRACKET) {
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

    switch (event.key.key) {
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

  switch (event.key.key) {
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

  switch (event.key.key) {
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

void WorkspaceShell::SyncTextInputSurface(SDL_Window* window) {
  const TextInputSurface current_surface = CurrentTextInputSurface();
  if (current_surface == active_text_input_surface_) {
    return;
  }

  active_text_input_surface_ = current_surface;
  text_composition_ = TextCompositionState{};
  SDL_Window* target_window = window != nullptr ? window : SDL_GetKeyboardFocus();
  if (target_window != nullptr) {
    SDL_ClearComposition(target_window);
  }
}

bool WorkspaceShell::CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const {
  if (text_composition_.text.empty() || text_composition_.surface != CurrentTextInputSurface() ||
      (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
    return false;
  }

  switch (key) {
    case SDLK_BACKSPACE:
    case SDLK_DELETE:
    case SDLK_ESCAPE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_TAB:
    case SDLK_UP:
    case SDLK_DOWN:
    case SDLK_LEFT:
    case SDLK_RIGHT:
    case SDLK_HOME:
    case SDLK_END:
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN:
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::HandleTextEditing(const SDL_TextEditingEvent& event) {
  if (menu_bar_open_ || tree_context_menu_.open) {
    text_composition_ = TextCompositionState{};
    return true;
  }
  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = CurrentTextInputSurface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    text_composition_ = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    text_composition_ = TextCompositionState{};
    return true;
  }

  text_composition_.surface = surface;
  text_composition_.text = event.text;
  text_composition_.start = event.start;
  text_composition_.length = event.length;
  return true;
}

bool WorkspaceShell::HandleTextInput(const SDL_TextInputEvent& event) {
  if (menu_bar_open_ || tree_context_menu_.open) {
    return true;
  }
  if (event.text == nullptr || event.text[0] == '\0' || dirty_prompt_visible_) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  text_composition_ = TextCompositionState{};
  const std::string_view input(event.text);
  if (prompt_surface_visible_ &&
      prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
    prompt_surface_state_.input.append(input);
    return true;
  }
  if (command_mode_) {
    command_input_.append(input);
    command_history_index_.reset();
    command_history_pending_input_.clear();
    ClearCommandCompletionFeedback();
    return true;
  }

  if (overlay_visible_) {
    switch (overlay_mode_) {
      case OverlayMode::CommitPicker:
        compare_picker_query_.append(input);
        RefreshComparePicker();
        return true;
      case OverlayMode::BufferSearch:
        buffer_search_query_.append(input);
        RefreshBufferSearch();
        return true;
      case OverlayMode::BufferReplace:
        if (buffer_search_field_ == BufferSearchField::Search) {
          buffer_search_query_.append(input);
          RefreshBufferSearch();
        } else {
          buffer_replace_text_.append(input);
        }
        return true;
      case OverlayMode::ProjectSearch:
        project_search_query_.append(input);
        RefreshProjectSearch();
        return true;
      case OverlayMode::FileFinder:
      default:
        file_finder_.AppendQueryText(input);
        ResetOverlayScroll();
        return true;
    }
  }

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_ && sidebar_mode_ == SidebarMode::Search &&
      project_search_editing_) {
    project_search_edit_buffer_.append(input);
    return true;
  }

  if (focus_ == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    editor::TextViewport* viewport = ActiveEditableViewport();
    if (viewport == nullptr) {
      return false;
    }
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    viewport->InsertText(input);
    if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
      RefreshCompareTabDerivedState(*compare_tab);
      SyncCompareSelectionFromViewport(*compare_tab, true);
    }
    if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, cursor_before);
    }
    ResetCaretBlink();
    return true;
  }

  if (focus_ == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    ClearTerminalSelection();
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      AppendTerminalPendingInput(input);
      terminal_tab->session.SendBytes(input);
    }
    return true;
  }

  return false;
}

bool WorkspaceShell::HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && TerminalHasSelection()) {
    const std::string text = SelectedTerminalText(terminal_tab->session.SnapshotLines());
    if (!text.empty() && WriteClipboardText(text)) {
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && TerminalHasSelection()) {
    ClearTerminalSelection();
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && (modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_V) {
    return PasteClipboardIntoTerminal();
  }

  if ((modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_INSERT) {
    return PasteClipboardIntoTerminal();
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key >= SDLK_A && event.key <= SDLK_Z) {
      const char control =
          static_cast<char>(1 + (event.key - SDLK_A));
      terminal_tab->session.SendBytes(std::string(1, control));
      return true;
    }
    switch (event.key) {
      case SDLK_LEFTBRACKET:
        terminal_tab->session.SendBytes("\x1b");
        return true;
      case SDLK_BACKSLASH:
        terminal_tab->session.SendBytes("\x1c");
        return true;
      case SDLK_RIGHTBRACKET:
        terminal_tab->session.SendBytes("\x1d");
        return true;
      case SDLK_SPACE:
        terminal_tab->session.SendBytes(std::string(1, '\0'));
        return true;
      default:
        break;
    }
  }

  if (modifiers & SDL_KMOD_ALT) {
    const char input_character = KeycodeToAscii(event.key, modifiers);
    if (input_character != '\0') {
      std::string bytes(1, '\x1b');
      bytes.push_back(input_character);
      terminal_tab->session.SendBytes(bytes);
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Escape);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      SubmitTerminalPendingInput();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Enter);
      return true;
    case SDLK_BACKSPACE:
      EraseLastTerminalPendingInputCodepoint();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Backspace);
      return true;
    case SDLK_TAB:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Tab);
      return true;
    case SDLK_UP:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Up);
      return true;
    case SDLK_DOWN:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Down);
      return true;
    case SDLK_RIGHT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Right);
      return true;
    case SDLK_LEFT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Left);
      return true;
    case SDLK_HOME:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Home);
      return true;
    case SDLK_END:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::End);
      return true;
    case SDLK_PAGEUP:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageUp);
      return true;
    case SDLK_PAGEDOWN:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageDown);
      return true;
    case SDLK_INSERT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Insert);
      return true;
    case SDLK_DELETE:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Delete);
      return true;
    default:
      break;
  }

  return false;
}

bool WorkspaceShell::PasteClipboardIntoTerminal() {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }

  const std::optional<std::string> clipboard_text = ReadClipboardText();
  if (!clipboard_text.has_value()) {
    return true;
  }

  ClearTerminalSelection();
  if (clipboard_text->find_first_of("\r\n") == std::string::npos) {
    AppendTerminalPendingInput(*clipboard_text);
  }
  terminal_tab->session.PasteText(*clipboard_text);
  return true;
}

}  // namespace microide::workspace
