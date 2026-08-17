#pragma once

#include <SDL3/SDL.h>

namespace microide::app {

// Owns the retained full-window "scene" render-target texture that the editor
// renders into so partial frames can re-present unchanged regions without
// re-rendering them. Encapsulates the texture lifetime (RAII), the validity
// flag, and the resize-drag coalescing that avoids reallocating the GPU target
// on every resize event.
//
// The texture is sized in DEVICE PIXELS and carries its own copy of the window's
// logical presentation. That distinction is the whole point: `Ensure` used to
// take the logical size, compare it against `SDL_GetRenderOutputSize` (pixels),
// and return false when they differed. They differ whenever the display scale or
// the UI scale is not 1.0 — so on every HiDPI display the texture was never
// created, `Application::Render` took its direct-to-window fallback every frame,
// and the entire partial-redraw path was dead code with nothing reporting it
// (measured on a scale=2.0 Wayland session: 100% of frames `fallback-full`).
class SceneTexturePresenter {
 public:
  SceneTexturePresenter() = default;
  ~SceneTexturePresenter() { Destroy(); }

  SceneTexturePresenter(const SceneTexturePresenter&) = delete;
  SceneTexturePresenter& operator=(const SceneTexturePresenter&) = delete;

  // Ensure a texture sized to the renderer's pixel output exists and is mapped
  // for the current (logical_width, logical_height) presentation. Returns false
  // (and renders should fall back to direct-to-window) when the output size
  // cannot be read, allocation fails, or a resize is still settling. A freshly
  // (re)allocated texture starts invalid; so does one whose logical grid changed
  // under it, since what it holds was laid out for the old one.
  bool Ensure(SDL_Renderer* renderer, int logical_width, int logical_height);

  // Call immediately after binding texture() as the render target. Copies the
  // window's logical presentation onto the target's own view so the shell's
  // logical-coordinate drawing lands on the texture's device pixels. Cheap and
  // idempotent: a target's presentation survives unbind/rebind, so this does
  // real work only on the first bind after a (re)allocation.
  void ApplyRenderTargetPresentation(SDL_Renderer* renderer);

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

  // Device-pixel size of the retained texture (0 when there is none). Exposed for
  // the regression test that pins "sized in pixels, not in logical units".
  int pixel_width() const { return pixel_width_; }
  int pixel_height() const { return pixel_height_; }

 private:
  // Coalesce scene-texture reallocation during active resize drags. While
  // resize events keep firing we render through the fallback path instead of
  // paying a GPU texture alloc per event; after this many nanoseconds without a
  // resize event the texture is rebuilt exactly once.
  static constexpr Uint64 kResizeSettleNs = 150ULL * 1'000'000ULL;

  SDL_Texture* texture_ = nullptr;
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  int logical_width_ = 0;
  int logical_height_ = 0;
  bool presentation_applied_ = false;
  bool valid_ = false;
  // Zero means "no recent resize"; the texture may (re)allocate immediately.
  Uint64 last_resize_event_ns_ = 0;
};

}  // namespace microide::app
