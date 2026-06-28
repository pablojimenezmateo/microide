#pragma once

#include <SDL3/SDL.h>

#include <string_view>

namespace microide::render {

// The SDL software renderer reports this exact driver name. Everything else
// SDL exposes (opengl, opengles2, vulkan, gpu, direct3d11/12, metal) is a
// GPU-accelerated backend.
inline constexpr std::string_view kSoftwareRendererName = "software";

// Driver name SDL selected for `renderer` (e.g. "opengl", "vulkan",
// "software"). Returns "none" for a null renderer and "unknown" if SDL has no
// name. The returned view points at SDL-owned storage that lives for the
// renderer's lifetime.
inline std::string_view RendererDriverName(SDL_Renderer* renderer) {
  if (renderer == nullptr) {
    return "none";
  }
  const char* name = SDL_GetRendererName(renderer);
  return name != nullptr ? std::string_view(name) : std::string_view("unknown");
}

// True when `renderer` is a GPU-accelerated backend. A null or software
// renderer is not GPU. This is the single gate the batched-text path keys on:
// SDL_RenderGeometry only wins on a GPU pipeline (on the software renderer it
// rasterizes per-pixel and regresses; see the glyph-atlas closeout guardrail).
inline bool RendererIsGpu(SDL_Renderer* renderer) {
  if (renderer == nullptr) {
    return false;
  }
  return RendererDriverName(renderer) != kSoftwareRendererName;
}

}  // namespace microide::render
