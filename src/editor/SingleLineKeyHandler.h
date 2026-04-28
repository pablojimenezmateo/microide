#pragma once

#include <functional>
#include <optional>
#include <string>

#include <SDL3/SDL.h>

namespace microide::editor {

class SingleLineEditor;

class SingleLineKeyHandler {
 public:
  struct Clipboard {
    std::function<void(const std::string&)> write_text;
    std::function<std::optional<std::string>()> read_text;
  };

  static bool HandleKeyDown(SingleLineEditor& editor,
                            SDL_Keycode key,
                            SDL_Keymod modifiers,
                            const Clipboard& clipboard);
};

}  // namespace microide::editor
