#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <utility>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceTextInputCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

KeyInputCoordinator::KeyInputCoordinator(ProjectWorkspaceState& state,
                                         PromptState& prompts,
                                         MenuSurfaceState& menu_state,
                                         Operations operations)
    : state_(state),
      prompts_(prompts),
      menu_state_(menu_state),
      operations_(std::move(operations)) {}

bool KeyInputCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!operations_.has_pending_redraw()) {
      request_redraw();
    }
  };

  const SDL_Keymod modifiers = event.mod != SDL_KMOD_NONE ? event.mod : SDL_GetModState();
  if (prompts_.dirty_visible) {
    const bool handled = HandleDirtyPromptKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_prompt_redraw(); });
    }
    return handled;
  }
  if (menu_state_.tree_context_menu.open) {
    const bool handled = HandleTreeContextMenuKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { operations_.request_chrome_redraw(); });
    }
    return handled;
  }
  if (menu_state_.menu_bar_open) {
    const bool handled = HandleMenuBarKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_chrome_redraw(); });
    }
    return handled;
  }
  if (operations_.text_input_composition_consumes_key(event.key, modifiers)) {
    return true;
  }
  if (prompts_.surface_visible) {
    const bool handled = HandlePromptSurfaceKeyDown(event);
    if (handled) {
      ensure_redraw([this]() { operations_.request_prompt_redraw(); });
    }
    return handled;
  }

  const bool active_compare_tab = operations_.active_tab_is_compare();
  const bool active_merge_tab = operations_.active_tab_is_merge();
  if (HandleGlobalKeyDown(event, modifiers, active_compare_tab, active_merge_tab)) {
    return true;
  }
  if (state_.panel.command_mode) {
    bool handled = operations_.command_prompt_handle_key_down(event);
    if (!handled) {
      handled = operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
    if (handled) {
      ensure_redraw([this]() { operations_.request_bottom_panel_command_redraw(); });
    }
    return handled;
  }
  if (HandleSurfaceNavigationKeyDown(event, modifiers)) {
    ensure_redraw([this]() { operations_.request_window_redraw(); });
    return true;
  }
  if (state_.surface.focus == FocusTarget::Overlay) {
    const bool handled = HandleOverlayKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_overlay_redraw(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible) {
    const bool handled = HandleSidebarKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_sidebar_redraw(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Panel &&
      state_.panel.content == PanelContentKind::Terminal &&
      operations_.active_terminal_tab() != nullptr) {
    const bool handled = operations_.text_input_handle_terminal_key_down(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_bottom_panel_content_redraw(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Editor && active_compare_tab) {
    const bool handled = HandleCompareKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_focused_editor_redraw(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Editor && active_merge_tab) {
    const bool handled = HandleMergeKeyDown(event, modifiers);
    if (handled) {
      ensure_redraw([this]() { operations_.request_focused_editor_redraw(); });
    }
    return handled;
  }

  const bool handled = HandleDefaultEditorKeyDown(event, modifiers);
  if (handled) {
    ensure_redraw([this]() { operations_.request_focused_editor_redraw(); });
  }
  return handled;
}

bool KeyInputCoordinator::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers,
                                              bool active_compare_tab,
                                              bool active_merge_tab) {
  if ((modifiers & SDL_KMOD_CTRL) != 0 && event.key == SDLK_V) {
    const bool surface_accepts_paste =
        state_.panel.command_mode ||
        (state_.overlay.visible &&
         (state_.overlay.mode == OverlayMode::FileFinder ||
          state_.overlay.mode == OverlayMode::CommitPicker ||
          state_.overlay.mode == OverlayMode::BufferSearch ||
          state_.overlay.mode == OverlayMode::BufferReplace ||
          state_.overlay.mode == OverlayMode::ProjectSearch)) ||
        (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible &&
         operations_.active_sidebar_mode() == SidebarMode::Search &&
         state_.overlay.workflow.project_search.editing);
    if (surface_accepts_paste) {
      return operations_.execute_action(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
    }
  }

  const TextInputSurface text_input_surface = operations_.current_text_input_surface();
  const bool single_line_text_surface =
      text_input_surface == TextInputSurface::PromptInput ||
      text_input_surface == TextInputSurface::Command ||
      text_input_surface == TextInputSurface::ChatComposer ||
      text_input_surface == TextInputSurface::FileFinder ||
      text_input_surface == TextInputSurface::BufferSearch ||
      text_input_surface == TextInputSurface::BufferReplaceSearch ||
      text_input_surface == TextInputSurface::BufferReplaceReplace ||
      text_input_surface == TextInputSurface::ProjectSearchOverlay ||
      text_input_surface == TextInputSurface::CommitPicker ||
      text_input_surface == TextInputSurface::SidebarSearchQuery ||
      text_input_surface == TextInputSurface::SidebarSearchReplace;
  if (single_line_text_surface && (modifiers & SDL_KMOD_CTRL) != 0) {
    switch (event.key) {
      case SDLK_A:
        return operations_.execute_action(ActionId::SelectAll, {}, ActionSource::Shortcut);
      case SDLK_C:
        return operations_.execute_action(ActionId::CopySelection, {}, ActionSource::Shortcut);
      case SDLK_X:
        return operations_.execute_action(ActionId::CutSelection, {}, ActionSource::Shortcut);
      default:
        break;
    }
  }

  const auto bindings = operations_.resolved_keybindings();
  const ResolvedKeybinding* binding =
      FindKeybinding(bindings, event.key, modifiers, ActiveKeybindingContext());
  if (binding == nullptr) {
    return false;
  }

  const bool editor_shortcut =
      binding->action == ActionId::SelectAll || binding->action == ActionId::Undo ||
      binding->action == ActionId::Redo || binding->action == ActionId::CopySelection ||
      binding->action == ActionId::CutSelection || binding->action == ActionId::PasteClipboard ||
      binding->action == ActionId::CloseActiveTab || binding->action == ActionId::Search ||
      binding->action == ActionId::ReplaceInBuffer || binding->action == ActionId::ProjectSearch;

  if (binding->action == ActionId::Tab && binding->args.empty() &&
      binding->command_name.empty() &&
      (state_.panel.command_mode || state_.overlay.visible)) {
    return false;
  }

  if (editor_shortcut &&
      (state_.panel.command_mode || state_.overlay.visible ||
       state_.surface.focus != FocusTarget::Editor)) {
    return false;
  }
  if ((binding->action == ActionId::Search || binding->action == ActionId::ReplaceInBuffer ||
       binding->action == ActionId::ProjectSearch) &&
      active_merge_tab) {
    return false;
  }
  if (binding->action == ActionId::SelectAll &&
      operations_.active_navigable_viewport() == nullptr) {
    return false;
  }
  if (binding->action == ActionId::Save && active_compare_tab) {
    return false;
  }
  if (!binding->command_name.empty() &&
      (state_.panel.command_mode || state_.overlay.visible)) {
    return false;
  }

  return DispatchResolvedKeybinding(*binding, ActionSource::Shortcut);
}

KeybindingContext KeyInputCoordinator::ActiveKeybindingContext() const {
  switch (state_.surface.focus) {
    case FocusTarget::Editor:
      return KeybindingContext::Editor;
    case FocusTarget::Sidebar:
      return KeybindingContext::Sidebar;
    case FocusTarget::Panel:
      return state_.panel.content == PanelContentKind::Terminal &&
                     operations_.active_terminal_tab() != nullptr
                 ? KeybindingContext::Terminal
                 : KeybindingContext::Global;
    case FocusTarget::Overlay:
    default:
      return KeybindingContext::Global;
  }
}

bool KeyInputCoordinator::DispatchResolvedKeybinding(const ResolvedKeybinding& binding,
                                                     ActionSource source) {
  if (!binding.command_name.empty()) {
    return operations_.execute_command_name(binding.command_name, binding.args, source);
  }
  return operations_.execute_action(binding.action, binding.args, source);
}

bool KeyInputCoordinator::HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event,
                                                         SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_TAB:
      if (modifiers & SDL_KMOD_CTRL) {
        if (state_.overlay.visible) {
          state_.surface.focus = FocusTarget::Overlay;
          return true;
        }
        const bool include_panel =
            state_.panel.content != PanelContentKind::None || state_.panel.command_mode;
        if (include_panel) {
          if (state_.sidebar.visible) {
            if (modifiers & SDL_KMOD_SHIFT) {
              state_.surface.focus = state_.surface.focus == FocusTarget::Sidebar
                                         ? FocusTarget::Panel
                                         : state_.surface.focus == FocusTarget::Panel
                                               ? FocusTarget::Editor
                                               : FocusTarget::Sidebar;
            } else {
              state_.surface.focus = state_.surface.focus == FocusTarget::Sidebar
                                         ? FocusTarget::Editor
                                         : state_.surface.focus == FocusTarget::Editor
                                               ? FocusTarget::Panel
                                               : FocusTarget::Sidebar;
            }
          } else {
            state_.surface.focus =
                state_.surface.focus == FocusTarget::Panel ? FocusTarget::Editor : FocusTarget::Panel;
          }
        } else if (state_.sidebar.visible && !(modifiers & SDL_KMOD_SHIFT)) {
          state_.surface.focus =
              state_.surface.focus == FocusTarget::Sidebar ? FocusTarget::Editor : FocusTarget::Sidebar;
        } else if (state_.sidebar.visible) {
          state_.surface.focus =
              state_.surface.focus == FocusTarget::Editor ? FocusTarget::Sidebar : FocusTarget::Editor;
        } else {
          state_.surface.focus = FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (operations_.settings_overlay_visible()) {
        operations_.close_settings_overlay();
        return true;
      }
      if (state_.overlay.visible) {
        operations_.dismiss_overlay(false);
        return true;
      }
      if (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible &&
          state_.sidebar.temporary && operations_.active_sidebar_mode() == SidebarMode::Search) {
        operations_.close_sidebar();
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

KeyInputCoordinator WorkspaceShell::MakeKeyInputCoordinator() {
  return KeyInputCoordinator(
      context_.current_project_state, context_.prompts, context_.menu_state,
      KeyInputCoordinator::Operations{
          .has_pending_redraw = [this]() { return pending_render_invalidation_.HasAnyRedraw(); },
          .request_prompt_redraw = [this]() { RequestPromptRedraw(); },
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
          .request_bottom_panel_command_redraw =
              [this]() { RequestBottomPanelCommandRedraw(); },
          .request_bottom_panel_content_redraw =
              [this]() { RequestBottomPanelContentRedraw(); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .command_prompt_handle_key_down =
              [this](const SDL_KeyboardEvent& event) {
                return MakeCommandPromptCoordinator().HandleKeyDown(event);
              },
          .text_input_composition_consumes_key =
              [this](SDL_Keycode key, SDL_Keymod modifiers) {
                return MakeTextInputCoordinator().CompositionConsumesKey(key, modifiers);
              },
          .current_text_input_surface =
              [this]() { return CurrentTextInputSurface(); },
          .text_input_handle_single_line_key_down =
              [this](const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
                return MakeTextInputCoordinator().HandleSingleLineKeyDown(event, modifiers);
              },
          .text_input_handle_terminal_key_down =
              [this](const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
                return MakeTextInputCoordinator().HandleTerminalKeyDown(event, modifiers);
              },
          .confirm_dirty_prompt = [this]() { ConfirmDirtyPrompt(); },
          .keycode_to_ascii =
              [](SDL_Keycode key, SDL_Keymod modifiers) {
                return WorkspaceShell::KeycodeToAscii(key, modifiers);
              },
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
          .next_enabled_tree_context_menu_item_index =
              [this](int current_index, int delta) {
                return MakeMenuCoordinator().NextEnabledTreeContextMenuItemIndex(current_index, delta);
              },
          .execute_tree_context_menu_item =
              [this](std::size_t index) {
                return MakeMenuCoordinator().ExecuteTreeContextMenuItem(index);
              },
          .close_menu_bar = [this]() { MakeMenuCoordinator().CloseMenuBar(); },
          .switch_menu_bar_menu =
              [this](int delta) { return MakeMenuCoordinator().SwitchMenuBarMenu(delta); },
          .move_active_menu_item =
              [this](int delta) { return MakeMenuCoordinator().MoveActiveMenuItem(delta); },
          .execute_menu_item =
              [this](MenuId id, std::size_t index) {
                return MakeMenuCoordinator().ExecuteMenuItem(id, index);
              },
          .dismiss_prompt_surface = [this](bool restore_focus) { DismissPromptSurface(restore_focus); },
          .confirm_prompt_surface = [this]() { ConfirmPromptSurface(); },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .execute_command_name =
              [this](std::string_view command_name,
                     const std::vector<std::string>& args,
                     ActionSource source) {
                return ExecuteCommandName(command_name, args, source);
              },
          .resolved_keybindings =
              [this]() {
                return ResolveKeybindings(plugin_runtime_.Host(), context_.disabled_keybinding_ids);
              },
          .open_untitled_tab = [this]() { return OpenUntitledTab(); },
          .active_tab_is_compare = [this]() { return ActiveTabIsCompare(); },
          .active_tab_is_merge = [this]() { return ActiveTabIsMerge(); },
          .active_navigable_viewport = [this]() { return ActiveNavigableViewport(); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .active_terminal_tab = [this]() { return ActiveTerminalTab(); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .settings_overlay_visible = [this]() { return settings_overlay_service_.Visible(); },
          .close_settings_overlay = [this]() { CloseSettingsOverlay(); },
          .close_sidebar = [this]() { CloseSidebar(); },
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .activate_overlay_selection =
              [this]() {
                ActivateOverlaySelection();
              },
          .move_compare_picker_selection = [this](int delta) { MoveComparePickerSelection(delta); },
          .refresh_compare_picker = [this]() { RefreshComparePicker(); },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .compute_overlay_rect = [this](const SDL_FRect& rect) { return ComputeOverlayRect(rect); },
          .reveal_overlay_selection = [this](const SDL_FRect& rect) { RevealOverlaySelection(rect); },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .refresh_buffer_search = [this]() { RefreshBufferSearch(); },
          .move_project_search_selection = [this](int delta) { MoveProjectSearchSelection(delta); },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .replace_all_buffer_search_matches = [this]() { ReplaceAllBufferSearchMatches(); },
          .replace_current_buffer_search_match = [this]() { ReplaceCurrentBufferSearchMatch(); },
          .move_file_finder_selection = [this](int delta) { MoveFileFinderSelection(delta); },
          .reset_overlay_scroll = [this]() { ResetOverlayScroll(); },
          .begin_project_search_edit =
              [this](ProjectSearchEditField field) { BeginProjectSearchEdit(field); },
          .commit_project_search_edit = [this]() { CommitProjectSearchEdit(); },
          .cancel_project_search_edit = [this]() { CancelProjectSearchEdit(); },
          .replace_all_project_search_matches = [this]() { ReplaceAllProjectSearchMatches(); },
          .toggle_project_search_pattern_mode = [this]() { ToggleProjectSearchPatternMode(); },
          .cycle_project_search_case_mode = [this]() { CycleProjectSearchCaseMode(); },
          .toggle_project_search_hidden_files = [this]() { ToggleProjectSearchHiddenFiles(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .restore_previous_sidebar = [this]() { RestorePreviousSidebar(); },
          .move_git_sidebar_selection = [this](int delta) { MoveGitSidebarSelection(delta); },
          .reveal_selected_git_sidebar_line = [this]() { RevealSelectedGitSidebarLine(); },
          .open_git_sidebar_entry =
              [this](std::size_t index) { return OpenGitSidebarEntry(index); },
          .stage_git_sidebar_entry =
              [this](std::size_t index) { return StageGitSidebarEntry(index); },
          .unstage_git_sidebar_entry =
              [this](std::size_t index) { return UnstageGitSidebarEntry(index); },
          .discard_git_sidebar_entry =
              [this](std::size_t index) { return DiscardGitSidebarEntry(index); },
          .move_problems_sidebar_selection = [this](int delta) { MoveProblemsSidebarSelection(delta); },
          .reveal_selected_problems_sidebar_line =
              [this]() { RevealSelectedProblemsSidebarLine(); },
          .open_selected_problem_sidebar_item = [this]() { return OpenSelectedProblemSidebarItem(); },
          .refresh_problems_sidebar = [this]() { return RefreshProblemsSidebar(); },
          .move_tests_sidebar_selection = [this](int delta) { MoveTestsSidebarSelection(delta); },
          .reveal_selected_tests_sidebar_line =
              [this]() { RevealSelectedTestsSidebarLine(); },
          .open_selected_test_sidebar_item =
              [this]() { return OpenSelectedTestSidebarItem(); },
          .run_selected_test_sidebar_item =
              [this]() { return RunSelectedTestSidebarItem(); },
          .refresh_tests_sidebar = [this]() { return RefreshTestsSidebar(); },
          .move_plugin_sidebar_selection = [this](int delta) { MovePluginSidebarSelection(delta); },
          .reveal_selected_plugin_sidebar_line = [this]() { RevealSelectedPluginSidebarLine(); },
          .open_selected_plugin_sidebar_item = [this]() { return OpenSelectedPluginSidebarItem(); },
          .refresh_plugin_sidebar = [this]() { return RefreshPluginSidebar(); },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .open_compare_picker = [this]() { OpenComparePicker(); },
          .move_compare_selection = [this](int delta) { MoveCompareSelection(delta); },
          .jump_compare_hunk = [this](int delta) { JumpCompareHunk(delta); },
          .open_working_file_from_compare = [this]() { OpenWorkingFileFromCompare(); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_change_redraw =
              [this](const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                RequestActiveEditableChangeRedraw(before_lines, after_lines);
              },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t first_line, std::size_t last_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(first_line, last_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .request_compare_row_range_redraw =
              [this](std::size_t first_row, std::size_t last_row) {
                RequestCompareRowRangeRedraw(first_row, last_row);
              },
          .request_close_active_tab =
              [this]() { RequestCloseTab(context_.current_project_state.active_tab_index); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .show_completion_overlay =
              [this](std::string* error_message) {
                return ShowCompletionOverlay(error_message);
              },
          .apply_selected_completion = [this]() { return ApplySelectedCompletion(); },
          .show_code_actions_overlay =
              [this](std::string* error_message) {
                return ShowCodeActionsOverlay(error_message);
              },
          .execute_selected_code_action =
              [this]() { return ExecuteSelectedCodeAction(); },
          .request_inline_completion =
              [this](std::string* error_message) {
                if (error_message != nullptr) {
                  *error_message = "Inline completion is retired";
                }
                return false;
              },
          .accept_inline_completion = [this]() { return false; },
          .dismiss_inline_completion = [this]() {},
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab,
                     const std::vector<std::string>& before_lines,
                     std::optional<editor::SelectionRange> selection_before,
                     editor::TextPosition cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab, before_lines, selection_before,
                                                     cursor_before);
              },
          .move_merge_selection = [this](int delta) { MoveMergeSelection(delta); },
          .apply_merge_choice = [this](compare::MergeChoice choice) { ApplyMergeChoice(choice); },
          .open_merge_result_file = [this]() { OpenMergeResultFile(); },
      });
}

}  // namespace microide::workspace
