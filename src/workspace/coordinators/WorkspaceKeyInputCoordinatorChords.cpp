#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

namespace microide::workspace {

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
