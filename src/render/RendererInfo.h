#pragma once

#include <SDL3/SDL.h>

#include <string_view>

namespace microide::render {

inline constexpr std::string_view kSoftwareRendererName = "software";

inline std::string_view RendererDriverName(SDL_Renderer* renderer) {
  if (renderer == nullptr) {
    return "none";
  }
  const char* name = SDL_GetRendererName(renderer);
  return name != nullptr ? std::string_view(name) : std::string_view("unknown");
}

inline bool RendererIsGpu(SDL_Renderer* renderer) {
  if (renderer == nullptr) {
    return false;
  }
  return RendererDriverName(renderer) != kSoftwareRendererName;
}

}
