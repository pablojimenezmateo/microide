#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

namespace microide::workspace {

bool KeyInputCoordinator::HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                                              SDL_Keymod modifiers,
                                              bool active_merge_tab) {
  if ((modifiers & SDL_KMOD_CTRL) != 0 && event.key == SDLK_V) {
    const bool surface_accepts_paste =
        (state_.overlay.visible &&
         (state_.overlay.mode == OverlayMode::FileFinder ||
          state_.overlay.mode == OverlayMode::CommitPicker ||
          state_.overlay.mode == OverlayMode::QuickPick ||
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
  if (const std::optional<bool> chord = HandleKeyChord(event, modifiers, key_ctx);
      chord.has_value()) {
    return *chord;
  }

  const ResolvedKeybinding* binding =
      FindKeybinding(bindings, event.key, modifiers, key_ctx, operations_.action_enabled);
  if (binding == nullptr) {
    return false;
  }

  const bool editor_shortcut =
      binding->action == ActionId::SelectAll || binding->action == ActionId::Undo ||
      binding->action == ActionId::Redo || binding->action == ActionId::CopySelection ||
      binding->action == ActionId::CutSelection || binding->action == ActionId::PasteClipboard ||
      binding->action == ActionId::Search || binding->action == ActionId::ReplaceInBuffer;

  if (binding->action == ActionId::Tab && binding->args.empty() &&
      binding->command_name.empty() && state_.overlay.visible) {
    return false;
  }

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

std::optional<bool> KeyInputCoordinator::HandleKeyChord(const SDL_KeyboardEvent& event,
                                                        SDL_Keymod modifiers,
                                                        KeybindingContext key_ctx) {
  const auto& bindings = operations_.resolved_keybindings();
  const bool chord_allowed = !state_.overlay.visible && key_ctx != KeybindingContext::Terminal;
  std::optional<ProjectSurfaceState::PendingChord>& pending = state_.surface.pending_chord;

  const bool modifier_only = event.key == SDLK_LCTRL || event.key == SDLK_RCTRL ||
                             event.key == SDLK_LSHIFT || event.key == SDLK_RSHIFT ||
                             event.key == SDLK_LALT || event.key == SDLK_RALT ||
                             event.key == SDLK_LGUI || event.key == SDLK_RGUI;
  if (pending.has_value() && modifier_only) {
    return true;
  }
  if (pending.has_value()) {
    const ProjectSurfaceState::PendingChord leader = *pending;
    pending.reset();
    if (!chord_allowed) {
      return std::nullopt;
    }
    const ResolvedKeybinding* binding = FindChordKeybinding(
        bindings, leader.key, leader.modifiers, event.key, modifiers, key_ctx, operations_.action_enabled);
    if (binding == nullptr) {
      return std::nullopt;
    }
    const bool typed = (NormalizedKeyModifiers(modifiers) & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
                       event.key >= 0x20 && event.key < 0x7F;
    if (typed && operations_.set_swallow_next_text_input) {
      operations_.set_swallow_next_text_input(true);
    }
    (void)DispatchResolvedKeybinding(*binding, ActionSource::Shortcut);
    return true;
  }
  if (chord_allowed && IsChordLeader(bindings, event.key, modifiers, key_ctx) &&
      FindKeybinding(bindings, event.key, modifiers, key_ctx) == nullptr) {
    pending = ProjectSurfaceState::PendingChord{event.key, NormalizedKeyModifiers(modifiers)};
    return true;
  }
  return std::nullopt;
}

}
