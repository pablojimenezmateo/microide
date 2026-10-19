#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

namespace microide::workspace {

// Keybinding dispatch: which context a key is pressed in, which resolved binding
// it runs (and the gates that refuse a matching one), and the Ctrl+K chords.
// tests/KeybindingReachabilityTests.cpp presses every built-in binding from every
// focus target against these rules.

bool KeyInputCoordinator::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers,
                                              bool active_merge_tab) {
  if ((modifiers & SDL_KMOD_CTRL) != 0 && event.key == SDLK_V) {
    const bool surface_accepts_paste =
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
  const bool single_line_text_surface = IsSingleLineTextInputSurface(text_input_surface);
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
  if (const std::optional<bool> chord = HandleCtrlKChord(event, modifiers, key_ctx);
      chord.has_value()) {
    return *chord;
  }

  const ResolvedKeybinding* binding = FindKeybinding(bindings, event.key, modifiers, key_ctx);
  if (binding == nullptr) {
    return false;
  }

  // Text shortcuts that act on the focused editor's buffer, whatever context a
  // user's keybindings file gives them. Workbench commands (close tab, find in
  // files) are not here: listing them made a Global rebinding of Ctrl+W or
  // Ctrl+Shift+F still dead outside the editor.
  const bool editor_shortcut =
      binding->action == ActionId::SelectAll || binding->action == ActionId::Undo ||
      binding->action == ActionId::Redo || binding->action == ActionId::CopySelection ||
      binding->action == ActionId::CutSelection || binding->action == ActionId::PasteClipboard ||
      binding->action == ActionId::Search || binding->action == ActionId::ReplaceInBuffer;

  if (binding->action == ActionId::Tab && binding->args.empty() &&
      binding->command_name.empty() && state_.overlay.visible) {
    return false;
  }

  // The find/replace widget is non-modal: while it floats and the editor holds
  // focus, editor shortcuts (Ctrl+F to re-focus the widget, Undo, Copy, …) must
  // still fire. Only a genuinely modal overlay blocks them.
  const bool modal_overlay_visible = state_.overlay.visible &&
                                     state_.overlay.mode != OverlayMode::BufferSearch &&
                                     state_.overlay.mode != OverlayMode::BufferReplace;
  if (editor_shortcut &&
      (modal_overlay_visible || state_.surface.focus != FocusTarget::Editor)) {
    return false;
  }
  if ((binding->action == ActionId::Search || binding->action == ActionId::ReplaceInBuffer) &&
      active_merge_tab) {
    return false;
  }
  if (binding->action == ActionId::SelectAll &&
      operations_.active_navigable_viewport() == nullptr) {
    return false;
  }
  if (!binding->command_name.empty() && state_.overlay.visible) {
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
                     operations_.active_terminal_pane() != nullptr
                 ? KeybindingContext::Terminal
                 : KeybindingContext::Global;
    case FocusTarget::Overlay:
    default:
      return KeybindingContext::Global;
  }
}

bool KeyInputCoordinator::DispatchResolvedKeybinding(const ResolvedKeybinding& binding,
                                                     ActionSource source) {
  dispatch_probe_.last = &binding;
  if (dispatch_probe_.record_only) {
    return true;
  }
  if (!binding.command_name.empty()) {
    return operations_.execute_command_name(binding.command_name, binding.args, source);
  }
  return operations_.execute_action(binding.action, binding.args, source);
}

// Ctrl+K chords. The leader works wherever workbench shortcuts do, as in VS Code:
// Ctrl+K Ctrl+O opens a folder and Ctrl+K Ctrl+<arrow> moves between editor panes
// from the tree or a panel too. It was editor-only, so from the tree the
// advertised chords did nothing. The fold chords act on the focused text and
// still need the editor. Answers whether the key was taken, or nullopt when it is
// not part of a chord and dispatch goes on.
std::optional<bool> KeyInputCoordinator::HandleCtrlKChord(const SDL_KeyboardEvent& event,
                                                          SDL_Keymod modifiers,
                                                          KeybindingContext key_ctx) {
  const bool chord_allowed = !state_.overlay.visible && key_ctx != KeybindingContext::Terminal;
  const bool editor_chord_allowed =
      chord_allowed && state_.surface.focus == FocusTarget::Editor &&
      key_ctx == KeybindingContext::Editor;

  // A modifier pressed on its own is not the chord's second key: Ctrl+K, let go of
  // Ctrl, Ctrl+Left is how the chord is often typed, and the fresh Ctrl press used
  // to cancel it (VS Code ignores modifier-only presses while a chord is pending).
  const bool modifier_only = event.key == SDLK_LCTRL || event.key == SDLK_RCTRL ||
                             event.key == SDLK_LSHIFT || event.key == SDLK_RSHIFT ||
                             event.key == SDLK_LALT || event.key == SDLK_RALT ||
                             event.key == SDLK_LGUI || event.key == SDLK_RGUI;
  if (state_.surface.editor_ctrl_k_leader_armed && modifier_only) {
    return true;
  }
  if (state_.surface.editor_ctrl_k_leader_armed) {
    state_.surface.editor_ctrl_k_leader_armed = false;
    if (chord_allowed) {
      const SDL_Keymod chord_mod = NormalizedKeyModifiers(modifiers);
      if (chord_mod == SDL_KMOD_CTRL && editor_chord_allowed) {
        if (event.key == SDLK_0 || event.key == SDLK_KP_0) {
          return operations_.execute_action(ActionId::FoldAll, {}, ActionSource::Shortcut);
        }
        if (event.key == SDLK_J) {
          return operations_.execute_action(ActionId::UnfoldAll, {}, ActionSource::Shortcut);
        }
        if (event.key == SDLK_I) {
          // VS Code's Show Hover: the hover card for the caret, by keyboard.
          return operations_.execute_action(ActionId::ShowHover, {}, ActionSource::Shortcut);
        }
      }
      // VS Code's git chords for a diff: Ctrl+K Ctrl+Alt+S stages the selected
      // ranges, Ctrl+K Ctrl+N unstages them, Ctrl+K Ctrl+R reverts them.
      if (chord_mod == (SDL_KMOD_CTRL | SDL_KMOD_ALT) && event.key == SDLK_S) {
        return operations_.execute_action(ActionId::GitStageSelectedRanges, {}, ActionSource::Shortcut);
      }
      if (chord_mod == SDL_KMOD_CTRL && (event.key == SDLK_N || event.key == SDLK_R)) {
        return operations_.execute_action(event.key == SDLK_N ? ActionId::GitUnstageSelectedRanges
                                                              : ActionId::GitRevertSelectedRanges,
                                          {}, ActionSource::Shortcut);
      }
      if (chord_mod == SDL_KMOD_CTRL) {
        if (event.key == SDLK_O) {
          // File > Open Folder / Project Tab… advertises Ctrl+K Ctrl+O (VS Code's
          // chord for the same command); the leader handler used to implement only
          // the two fold sequences, so the advertised chord did nothing.
          return operations_.execute_action(ActionId::ProjectOpen, {}, ActionSource::Shortcut);
        }
      }
      // VS Code's editor-grid chords: Ctrl+K Ctrl+<arrow> focuses the pane that
      // way, Ctrl+K Ctrl+Shift+<arrow> moves the focused pane there. Both are
      // no-ops with one pane, or when nothing lies in that direction.
      if (const bool move_pane = chord_mod == (SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
          move_pane || chord_mod == SDL_KMOD_CTRL) {
        switch (event.key) {
          case SDLK_LEFT:
            return operations_.execute_action(
                move_pane ? ActionId::MoveEditorGroupLeft : ActionId::FocusEditorGroupLeft, {},
                ActionSource::Shortcut);
          case SDLK_RIGHT:
            return operations_.execute_action(
                move_pane ? ActionId::MoveEditorGroupRight : ActionId::FocusEditorGroupRight, {},
                ActionSource::Shortcut);
          case SDLK_UP:
            return operations_.execute_action(
                move_pane ? ActionId::MoveEditorGroupUp : ActionId::FocusEditorGroupUp, {},
                ActionSource::Shortcut);
          case SDLK_DOWN:
            return operations_.execute_action(
                move_pane ? ActionId::MoveEditorGroupDown : ActionId::FocusEditorGroupDown, {},
                ActionSource::Shortcut);
          default:
            break;
        }
      }
    }
  } else if (chord_allowed && event.key == SDLK_K &&
             NormalizedKeyModifiers(modifiers) == SDL_KMOD_CTRL) {
    if (FindKeybinding(operations_.resolved_keybindings(), SDLK_K, modifiers, key_ctx) ==
        nullptr) {
      state_.surface.editor_ctrl_k_leader_armed = true;
      return true;
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace
