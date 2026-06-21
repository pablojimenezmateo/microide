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

  const bool clears_editor_ctrl_k_chord =
      prompts_.dirty_visible || menu_state_.tree_context_menu.open ||
      menu_state_.menu_bar_open || prompts_.surface_visible || state_.overlay.visible ||
      state_.panel.command_mode || state_.surface.focus != FocusTarget::Editor;
  if (clears_editor_ctrl_k_chord) {
    state_.surface.editor_ctrl_k_leader_armed = false;
  }

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
  // Settings / Help is a modal overlay: while it is visible it owns all keyboard
  // input so keystrokes cannot leak into (and edit) the surface underneath. The
  // two-pane Settings view is fully keyboard navigable; Help/About is read-only.
  // Every key is consumed regardless so nothing reaches the editor below.
  if (operations_.settings_overlay_visible()) {
    const auto overlay_redraw = [&]() {
      ensure_redraw([this]() { operations_.request_overlay_redraw(); });
    };
    if (event.key == SDLK_ESCAPE) {
      operations_.close_settings_overlay();
      ensure_redraw([this]() { operations_.request_window_redraw(); });
      return true;
    }
    if (!operations_.settings_overlay_is_settings_mode()) {
      return true;  // Help / About: read-only apart from Escape.
    }
    if (event.key == SDLK_TAB) {
      operations_.settings_cycle_focus((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
      overlay_redraw();
      return true;
    }
    constexpr int kFilterPane = 0;
    constexpr int kCategoryPane = 1;
    constexpr int kValuePane = 2;
    const int pane = operations_.settings_focused_pane();
    if (pane == kFilterPane) {
      if (event.key == SDLK_DOWN || event.key == SDLK_UP) {
        operations_.settings_focus_pane(kValuePane);
        operations_.settings_move_row(event.key == SDLK_DOWN ? 1 : -1);
      } else {
        operations_.text_input_handle_single_line_key_down(event, modifiers);
      }
      overlay_redraw();
      return true;
    }
    if (pane == kCategoryPane) {
      switch (event.key) {
        case SDLK_DOWN:
          operations_.settings_move_category(1);
          break;
        case SDLK_UP:
          operations_.settings_move_category(-1);
          break;
        case SDLK_RIGHT:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          operations_.settings_focus_pane(kValuePane);
          break;
        case SDLK_LEFT:
          operations_.settings_focus_pane(kFilterPane);
          break;
        default:
          break;
      }
      overlay_redraw();
      return true;
    }
    // Value pane.
    switch (event.key) {
      case SDLK_DOWN:
        operations_.settings_move_row(1);
        break;
      case SDLK_UP:
        operations_.settings_move_row(-1);
        break;
      case SDLK_RIGHT:
      case SDLK_EQUALS:
      case SDLK_PLUS:
      case SDLK_KP_PLUS:
        operations_.settings_step_selected(1);
        break;
      case SDLK_LEFT:
      case SDLK_MINUS:
      case SDLK_KP_MINUS:
        operations_.settings_step_selected(-1);
        break;
      case SDLK_SPACE:
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.settings_toggle_or_activate_selected();
        break;
      case SDLK_DELETE:
        operations_.settings_reset_selected();
        break;
      default:
        break;
    }
    overlay_redraw();
    return true;
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

  // The Variables surface owns its keys while the debug pane holds focus so tree
  // navigation + inline edit are not stolen by global shortcuts. Unhandled keys
  // fall through (the handler returns false) so global bindings like F5 still work.
  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::Variables) {
    if (HandleDebugVariablesKeyDown(event, modifiers)) {
      ensure_redraw([this]() { operations_.request_window_redraw(); });
      return true;
    }
  }

  // The Watch surface likewise owns its keys while focused (tree nav + inline edit,
  // plus add/edit/remove of watch expressions).
  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::Watch) {
    if (HandleDebugWatchKeyDown(event, modifiers)) {
      ensure_redraw([this]() { operations_.request_window_redraw(); });
      return true;
    }
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
          state_.overlay.mode == OverlayMode::LaunchConfigPicker ||
          state_.overlay.mode == OverlayMode::CommandPalette ||
          state_.overlay.mode == OverlayMode::BufferSearch ||
          state_.overlay.mode == OverlayMode::BufferReplace ||
          state_.overlay.mode == OverlayMode::ProjectSearch)) ||
        (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible &&
         operations_.active_sidebar_mode() == SidebarMode::Search &&
         state_.overlay.workflow.project_search.editing) ||
        (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible &&
         operations_.active_sidebar_mode() == SidebarMode::Git &&
         state_.sidebar.git.commit_workflow.open &&
         state_.sidebar.git.commit_workflow.focus_field == CommitWorkflowFocusField::Subject);
    if (surface_accepts_paste) {
      return operations_.execute_action(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
    }
  }

  const TextInputSurface text_input_surface = operations_.current_text_input_surface();
  const bool single_line_text_surface =
      text_input_surface == TextInputSurface::PromptInput ||
      text_input_surface == TextInputSurface::Command ||
      text_input_surface == TextInputSurface::FileFinder ||
      text_input_surface == TextInputSurface::BufferSearch ||
      text_input_surface == TextInputSurface::BufferReplaceSearch ||
      text_input_surface == TextInputSurface::BufferReplaceReplace ||
      text_input_surface == TextInputSurface::ProjectSearchOverlay ||
      text_input_surface == TextInputSurface::CommitPicker ||
      text_input_surface == TextInputSurface::SidebarSearchQuery ||
      text_input_surface == TextInputSurface::SidebarSearchReplace ||
      text_input_surface == TextInputSurface::CommitSubject;
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

  const auto& bindings = operations_.resolved_keybindings();
  const KeybindingContext key_ctx = ActiveKeybindingContext();
  const bool editor_chord_allowed =
      state_.surface.focus == FocusTarget::Editor && !state_.overlay.visible &&
      !state_.panel.command_mode && key_ctx == KeybindingContext::Editor;

  if (state_.surface.editor_ctrl_k_leader_armed) {
    state_.surface.editor_ctrl_k_leader_armed = false;
    if (editor_chord_allowed) {
      const SDL_Keymod chord_mod = NormalizedKeyModifiers(modifiers);
      if (chord_mod == SDL_KMOD_CTRL) {
        if (event.key == SDLK_0 || event.key == SDLK_KP_0) {
          return operations_.execute_action(ActionId::FoldAll, {}, ActionSource::Shortcut);
        }
        if (event.key == SDLK_J) {
          return operations_.execute_action(ActionId::UnfoldAll, {}, ActionSource::Shortcut);
        }
      }
    }
  } else if (editor_chord_allowed && event.key == SDLK_K &&
             NormalizedKeyModifiers(modifiers) == SDL_KMOD_CTRL) {
    if (FindKeybinding(bindings, SDLK_K, modifiers, KeybindingContext::Editor) == nullptr) {
      state_.surface.editor_ctrl_k_leader_armed = true;
      return true;
    }
  }

  const ResolvedKeybinding* binding = FindKeybinding(bindings, event.key, modifiers, key_ctx);
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

  // The find/replace widget is non-modal: while it floats and the editor holds
  // focus, editor shortcuts (Ctrl+F to re-focus the widget, Undo, Copy, …) must
  // still fire. Only a genuinely modal overlay blocks them.
  const bool modal_overlay_visible = state_.overlay.visible &&
                                     state_.overlay.mode != OverlayMode::BufferSearch &&
                                     state_.overlay.mode != OverlayMode::BufferReplace;
  if (editor_shortcut &&
      (state_.panel.command_mode || modal_overlay_visible ||
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

bool KeyInputCoordinator::HandleDebugVariablesKeyDown(const SDL_KeyboardEvent& event,
                                                      SDL_Keymod modifiers) {
  DebugVariablesModel& model = state_.debug_variables;
  if (model.IsEditing()) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (operations_.commit_debug_variable_edit) {
          operations_.commit_debug_variable_edit();
        }
        return true;
      case SDLK_ESCAPE:
        if (operations_.cancel_debug_variable_edit) {
          operations_.cancel_debug_variable_edit();
        }
        return true;
      default:
        // Field navigation/editing (arrows, home/end, backspace, …). Consume the
        // key regardless so no global binding fires mid-edit; text insertion
        // arrives separately via SDL_TEXTINPUT.
        operations_.text_input_handle_single_line_key_down(event, modifiers);
        return true;
    }
  }

  const std::vector<DebugVariableRowView>& rows = model.Rows();
  const std::size_t selected = model.SelectedRow();
  const DebugVariableRowView* row = selected < rows.size() ? &rows[selected] : nullptr;
  switch (event.key) {
    case SDLK_UP:
      model.MoveSelection(-1);
      return true;
    case SDLK_DOWN:
      model.MoveSelection(1);
      return true;
    case SDLK_RIGHT:
      if (row != nullptr && row->has_children && !row->expanded &&
          operations_.toggle_debug_variable_row) {
        operations_.toggle_debug_variable_row(selected);
      }
      return true;
    case SDLK_LEFT:
      if (row != nullptr && row->has_children && row->expanded &&
          operations_.toggle_debug_variable_row) {
        operations_.toggle_debug_variable_row(selected);
      }
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (row != nullptr) {
        if (row->has_children) {
          if (operations_.toggle_debug_variable_row) {
            operations_.toggle_debug_variable_row(selected);
          }
        } else if (row->editable && operations_.begin_debug_variable_edit) {
          operations_.begin_debug_variable_edit(selected);
        }
      }
      return true;
    default:
      return false;
  }
}

bool KeyInputCoordinator::HandleDebugWatchKeyDown(const SDL_KeyboardEvent& event,
                                                  SDL_Keymod modifiers) {
  DebugWatchModel& model = state_.debug_watch;
  if (model.IsEditing()) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (operations_.commit_debug_watch_edit) {
          operations_.commit_debug_watch_edit();
        }
        return true;
      case SDLK_ESCAPE:
        if (operations_.cancel_debug_watch_edit) {
          operations_.cancel_debug_watch_edit();
        }
        return true;
      default:
        operations_.text_input_handle_single_line_key_down(event, modifiers);
        return true;
    }
  }

  const std::vector<DebugVariableRowView>& rows = model.Rows();
  const std::size_t selected = model.SelectedRow();
  const DebugVariableRowView* row = selected < rows.size() ? &rows[selected] : nullptr;
  const std::optional<std::size_t> expr_index =
      row != nullptr ? model.ExpressionIndexForRow(selected) : std::nullopt;
  switch (event.key) {
    case SDLK_UP:
      model.MoveSelection(-1);
      return true;
    case SDLK_DOWN:
      model.MoveSelection(1);
      return true;
    case SDLK_RIGHT:
      if (row != nullptr && row->has_children && !row->expanded &&
          operations_.toggle_debug_watch_row) {
        operations_.toggle_debug_watch_row(selected);
      }
      return true;
    case SDLK_LEFT:
      if (row != nullptr && row->has_children && row->expanded &&
          operations_.toggle_debug_watch_row) {
        operations_.toggle_debug_watch_row(selected);
      }
      return true;
    case SDLK_INSERT:
      if (operations_.add_debug_watch_expression) {
        operations_.add_debug_watch_expression();
      }
      return true;
    case SDLK_DELETE:
      if (expr_index.has_value() && operations_.remove_debug_watch_expression) {
        operations_.remove_debug_watch_expression(*expr_index);
      }
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (expr_index.has_value()) {
        // Enter on an expression root edits its expression string.
        if (operations_.edit_debug_watch_expression) {
          operations_.edit_debug_watch_expression(*expr_index);
        }
      } else if (row != nullptr) {
        if (row->has_children) {
          if (operations_.toggle_debug_watch_row) {
            operations_.toggle_debug_watch_row(selected);
          }
        } else if (row->editable && operations_.begin_debug_watch_edit) {
          operations_.begin_debug_watch_edit(selected);
        }
      }
      return true;
    default:
      return false;
  }
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
      // Settings / Help Escape is handled by the modal trap in HandleKeyDown before
      // this navigation path runs, so it no longer needs a branch here.
      if (state_.overlay.visible) {
        // The find/replace widget is non-modal and owns its own Esc semantics
        // (close + return focus to the editor it floats over, preserving any folds
        // the search auto-revealed). Let it fall through to its mode handler — when
        // the editor (not the widget) holds focus, the editor's Esc handler closes
        // it instead.
        if (state_.overlay.mode == OverlayMode::BufferSearch ||
            state_.overlay.mode == OverlayMode::BufferReplace) {
          break;
        }
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
          .resolved_keybindings = [this]() -> const std::vector<ResolvedKeybinding>& {
            return ResolvedKeybindings();
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
          .settings_overlay_is_settings_mode =
              [this]() {
                return settings_overlay_service_.Mode() == SettingsOverlayMode::Settings;
              },
          .settings_focused_pane =
              [this]() { return static_cast<int>(settings_overlay_service_.FocusedPane()); },
          .settings_focus_pane =
              [this](int pane) {
                settings_overlay_service_.SetFocusedPane(static_cast<SettingsPane>(pane));
                InvalidateCursorKindFingerprint();
              },
          .settings_cycle_focus =
              [this](int delta) {
                settings_overlay_service_.CycleFocusedPane(delta);
                InvalidateCursorKindFingerprint();
              },
          .settings_move_category =
              [this](int delta) {
                settings_overlay_service_.MoveCategory(delta);
                EnsureSettingsSelectionVisible();
              },
          .settings_move_row =
              [this](int delta) {
                settings_overlay_service_.MoveRow(delta);
                EnsureSettingsSelectionVisible();
              },
          .settings_step_selected =
              [this](int direction) {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr) {
                  StepSetting(row->id, direction >= 0);
                  RefreshSettingsOverlayCatalog();
                }
              },
          .settings_toggle_or_activate_selected =
              [this]() {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr) {
                  StepSetting(row->id, true);
                  RefreshSettingsOverlayCatalog();
                }
              },
          .settings_reset_selected =
              [this]() {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr && row->resettable) {
                  ResetSettingValue(row->id);
                  RefreshSettingsOverlayCatalog();
                }
              },
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
          .seed_buffer_search_from_project_search =
              [this]() { OpenBufferSearchFromProjectSearchResult(); },
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
          .dispatch_git_sidebar_action =
              [this](GitSidebarActionId action, std::size_t index) {
                return DispatchGitSidebarAction(action, index);
              },
          .close_commit_workflow = [this]() { CloseCommitWorkflow(); },
          .request_commit_workflow_commit =
              [this]() {
                InitializeCommitWorkflowService();
                return commit_workflow_service_.RequestCommit(
                    context_.current_project_state.sidebar.git.commit_workflow,
                    project::CommitOperationKind::Create);
              },
          .set_commit_workflow_focus_field =
              [this](CommitWorkflowFocusField field) {
                auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
                workflow.focus_field = field;
                // Field switch is the coarse refresh point: re-run pre-checks and persist
                // the draft now rather than on every keystroke.
                InitializeCommitWorkflowService();
                commit_workflow_service_.OnDraftEdited(workflow);
                ResetCaretBlink();
                RequestSidebarRedraw();
              },
          .commit_body_write_clipboard_text =
              [this](std::string_view text) { return WriteClipboardText(text); },
          .commit_body_read_clipboard_text = [this]() { return ReadClipboardText(); },
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
          .stage_compare_hunk = [this]() { StageCompareHunk(); },
          .stage_compare_selected_lines = [this]() { StageCompareSelectedLines(); },
          .unstage_compare_hunk = [this]() { UnstageCompareHunk(); },
          .unstage_compare_selected_lines = [this]() { UnstageCompareSelectedLines(); },
          .open_discard_compare_hunk_prompt = [this]() { OpenDiscardCompareHunkPrompt(); },
          .open_discard_compare_selected_lines_prompt =
              [this]() { OpenDiscardCompareSelectedLinesPrompt(); },
          .open_working_file_from_compare = [this]() { OpenWorkingFileFromCompare(); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .mark_active_editor_folding_dirty =
              [this]() {
                if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
                  editor_tab->folding_model->MarkDirty();
                }
              },
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
              [this]() { RequestCloseTab(context_.current_project_state.focused_group().active_tab_index); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .show_completion_overlay =
              [this](std::string* error_message) {
                return assist_service_.ShowCompletionOverlay(error_message);
              },
          .apply_selected_completion = [this]() { return assist_service_.ApplySelectedCompletion(); },
          .show_code_actions_overlay =
              [this](std::string* error_message) {
                return assist_service_.ShowCodeActionsOverlay(error_message);
              },
          .execute_selected_code_action =
              [this]() { return assist_service_.ExecuteSelectedCodeAction(); },
          .request_inline_completion =
              [this](std::string* error_message) {
                if (error_message != nullptr) {
                  *error_message = "Inline completion is retired";
                }
                return false;
              },
          .accept_inline_completion = [this]() { return false; },
          .dismiss_inline_completion = [this]() {},
          .try_snippet_tab_in_editor =
              [this](bool shift_tab) { return assist_service_.TrySnippetTabInEditor(shift_tab); },
          .try_snippet_escape_in_editor = [this]() { return assist_service_.TrySnippetEscapeInEditor(); },
          .try_snippet_backspace_in_editor =
              [this](editor::TextViewport* viewport) {
                return assist_service_.TrySnippetBackspaceInEditor(viewport);
              },
          .try_snippet_delete_forward_in_editor =
              [this](editor::TextViewport* viewport) {
                return assist_service_.TrySnippetDeleteForwardInEditor(viewport);
              },
          .notify_snippet_session_caret_moved =
              [this]() { assist_service_.NotifySnippetSessionCaretMoved(); },
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
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
          .toggle_debug_variable_row =
              [this](std::size_t row) { debug_service_.ToggleVariableRow(row); },
          .begin_debug_variable_edit =
              [this](std::size_t row) { debug_service_.BeginVariableEdit(row); },
          .commit_debug_variable_edit = [this]() { debug_service_.CommitVariableEdit(); },
          .cancel_debug_variable_edit = [this]() { debug_service_.CancelVariableEdit(); },
          .toggle_debug_watch_row = [this](std::size_t row) { debug_service_.ToggleWatchRow(row); },
          .begin_debug_watch_edit = [this](std::size_t row) { debug_service_.BeginWatchEdit(row); },
          .commit_debug_watch_edit = [this]() { debug_service_.CommitWatchEdit(); },
          .cancel_debug_watch_edit = [this]() { debug_service_.CancelWatchEdit(); },
          .add_debug_watch_expression = [this]() { OpenWatchExpressionPrompt(std::nullopt); },
          .edit_debug_watch_expression =
              [this](std::size_t index) { OpenWatchExpressionPrompt(index); },
          .remove_debug_watch_expression =
              [this](std::size_t index) { debug_service_.RemoveWatch(index); },
      });
}

}  // namespace microide::workspace
