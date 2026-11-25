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
      .get_png = []() -> std::optional<std::string> {
            if (!SDL_HasClipboardData("image/png")) {
              return std::nullopt;
            }
            std::size_t size = 0;
            void* data = SDL_GetClipboardData("image/png", &size);
            if (data == nullptr) {
              return std::nullopt;
            }
            std::string bytes(static_cast<const char*>(data), size);
            SDL_free(data);
            return bytes;
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
    held_.reset();
    system_at_hold_.reset();
  }
  return system;
}

std::optional<std::string> SystemClipboard::ReadPng() {
  std::optional<std::string> bytes = backend_.get_png ? backend_.get_png() : std::nullopt;
  constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";
  if (!bytes.has_value() || bytes->size() > kMaxPngBytes || !bytes->starts_with(kPngSignature)) {
    return std::nullopt;
  }
  return bytes;
}

}
