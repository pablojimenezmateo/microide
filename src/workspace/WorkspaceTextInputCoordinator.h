#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::TextInputCoordinator {
 public:
  explicit TextInputCoordinator(WorkspaceShell& shell);

  void SyncTextInputSurface(SDL_Window* window);
  bool CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const;
  bool HandleTextEditing(const SDL_TextEditingEvent& event);
  bool HandleTextInput(const SDL_TextInputEvent& event);
  bool HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool PasteClipboardIntoTerminal();

 private:
  void RequestCompositionRedraw(TextInputSurface surface);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
