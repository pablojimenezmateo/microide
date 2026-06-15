#pragma once

#include <SDL3/SDL.h>

namespace microide::app {

// Owns the retained full-window "scene" render-target texture that the editor
// renders into so partial frames can re-present unchanged regions without
// re-rendering them. Encapsulates the texture lifetime (RAII), the validity
// flag, and the resize-drag coalescing that avoids reallocating the GPU target
// on every resize event.
class SceneTexturePresenter {
 public:
  SceneTexturePresenter() = default;
  ~SceneTexturePresenter() { Destroy(); }

  SceneTexturePresenter(const SceneTexturePresenter&) = delete;
  SceneTexturePresenter& operator=(const SceneTexturePresenter&) = delete;

  // Ensure a texture sized to (logical_width, logical_height) exists and matches
  // the renderer output size. Returns false (and renders should fall back to
  // direct-to-window) when the size mismatches, allocation fails, or a resize is
  // still settling. A freshly (re)allocated texture starts invalid.
  bool Ensure(SDL_Renderer* renderer, int logical_width, int logical_height);

  // Destroy the texture and reset state. Must be called before the owning
  // renderer is destroyed.
  void Destroy();

  // Record that a window-resize / pixel-size-change event just occurred so the
  // next Ensure() coalesces reallocation until the drag settles.
  void NoteResizeEvent(Uint64 now_ns) { last_resize_event_ns_ = now_ns; }

  SDL_Texture* texture() const { return texture_; }
  bool valid() const { return valid_; }
  void MarkValid() { valid_ = true; }
  void Invalidate() { valid_ = false; }

 private:
  // Coalesce scene-texture reallocation during active resize drags. While
  // resize events keep firing we render through the fallback path instead of
  // paying a GPU texture alloc per event; after this many nanoseconds without a
  // resize event the texture is rebuilt exactly once.
  static constexpr Uint64 kResizeSettleNs = 150ULL * 1'000'000ULL;

  SDL_Texture* texture_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool valid_ = false;
  // Zero means "no recent resize"; the texture may (re)allocate immediately.
  Uint64 last_resize_event_ns_ = 0;
};

}  // namespace microide::app
