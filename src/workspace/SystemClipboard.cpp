#include "workspace/SystemClipboard.h"

#include <SDL3/SDL.h>

namespace microide::workspace {

SystemClipboard::SystemClipboard()
    : backend_{
          .set = [](std::string_view text) { return SDL_SetClipboardText(std::string(text).c_str()); },
          .get = []() -> std::optional<std::string> {
            char* text = SDL_GetClipboardText();
            if (text == nullptr) {
              return std::nullopt;
            }
            std::string copied(text);
            SDL_free(text);
            return copied;
          },
      } {}

bool SystemClipboard::Write(std::string_view text) {
  const bool set = backend_.set && backend_.set(text);
  std::optional<std::string> now = backend_.get ? backend_.get() : std::nullopt;
  if (set && now.has_value() && *now == text) {
    held_.reset();
    system_at_hold_.reset();
    return true;
  }
  // Refused, or reported set and not there: keep it for this app's paste.
  held_ = std::string(text);
  system_at_hold_ = std::move(now);
  return true;
}

std::optional<std::string> SystemClipboard::Read() {
  std::optional<std::string> system = backend_.get ? backend_.get() : std::nullopt;
  if (held_.has_value()) {
    if (system == system_at_hold_) {
      return held_;
    }
    held_.reset();  // the system clipboard moved on: another copy took over
    system_at_hold_.reset();
  }
  return system;
}

}  // namespace microide::workspace
