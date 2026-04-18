#include "workspace/WorkspaceKeyInputCoordinator.h"

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

bool WorkspaceShell::KeyInputCoordinator::HandleOverlayKeyDown(const SDL_KeyboardEvent& event,
                                                               SDL_Keymod modifiers) {
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

bool WorkspaceShell::KeyInputCoordinator::HandleSidebarKeyDown(const SDL_KeyboardEvent& event,
                                                               SDL_Keymod modifiers) {
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
          const auto& result = shell_.overlay_workflow_.project_search
                                   .results[shell_.overlay_workflow_.project_search.selected_index];
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
          shell_.problems_sidebar_.selected_index =
              shell_.problems_sidebar_.entries.size() - 1;
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

}  // namespace microide::workspace
