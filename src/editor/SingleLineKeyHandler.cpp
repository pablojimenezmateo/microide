#include "editor/SingleLineKeyHandler.h"

#include "editor/SingleLineEditor.h"

namespace microide::editor {

namespace {

bool HasAccel(SDL_Keymod modifiers) {
  return (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
}

}  // namespace

bool SingleLineKeyHandler::HandleKeyDown(SingleLineEditor& editor,
                                         SDL_Keycode key,
                                         SDL_Keymod modifiers,
                                         const Clipboard& clipboard) {
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  if (HasAccel(modifiers)) {
    switch (key) {
      case SDLK_A:
        return editor.SelectAll();
      case SDLK_C: {
        const std::string selection = editor.CopySelection();
        if (selection.empty()) {
          return false;
        }
        if (clipboard.write_text) {
          clipboard.write_text(selection);
        }
        return true;
      }
      case SDLK_X: {
        const std::optional<std::string> cut_text = editor.CutSelection();
        if (!cut_text.has_value()) {
          return false;
        }
        if (clipboard.write_text) {
          clipboard.write_text(*cut_text);
        }
        return true;
      }
      case SDLK_V: {
        if (!clipboard.read_text) {
          return false;
        }
        const auto pasted = clipboard.read_text();
        return pasted.has_value() && editor.Paste(*pasted);
      }
      case SDLK_LEFT:
        return editor.MoveHome(extend_selection);
      case SDLK_RIGHT:
        return editor.MoveEnd(extend_selection);
      default:
        break;
    }
  }

  switch (key) {
    case SDLK_BACKSPACE:
      return editor.Backspace();
    case SDLK_DELETE:
      return editor.DeleteForward();
    case SDLK_LEFT:
      return editor.MoveLeft(extend_selection);
    case SDLK_RIGHT:
      return editor.MoveRight(extend_selection);
    case SDLK_HOME:
      return editor.MoveHome(extend_selection);
    case SDLK_END:
      return editor.MoveEnd(extend_selection);
    default:
      return false;
  }
}

}  // namespace microide::editor
