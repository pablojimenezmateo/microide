#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

// Applies `clip`, narrowed to any outer clip, to the renderer for the lifetime of
// the scope, then restores the renderer's PREVIOUS clip state exactly — the
// enabled rect if one was set, or the disabled state if none was.
//
// This exists because the naive "save, set, restore-to-nullptr" pattern silently
// breaks two ways:
//   * SDL_GetRenderClipRect reports success/failure, not whether a clip is active
//     (it fills an empty rect when clipping is disabled), so it cannot be used to
//     decide whether to restore. Use SDL_RenderClipEnabled for that.
//   * SDL_SetRenderClipRect ENABLES a zero-area clip for a {0,0,0,0} rect; only a
//     NULL pointer disables clipping. So restoring a saved-but-empty rect blanks
//     everything drawn afterward, and restoring to nullptr drops an outer clip
//     (e.g. the dirty-region rect a partial repaint relies on) instead of keeping
//     it — letting later draws escape the dirty region.
// `clip` narrowed to the renderer's current clip, if one is set. A nested clip
// must never WIDEN the outer one: a partial repaint runs the whole scene under
// the dirty rect's clip, and a surface that simply set its own clip drew outside
// that rect -- over pixels that were already right. Opaque fills come out the
// same; anti-aliased text blended over itself does not, so a banner message got
// bolder on every partial frame that ran its paint code without covering it
// (TD-2026-10-15-341 found it). An empty intersection is a zero-area clip, which
// SDL enables and which draws nothing -- what the caller should get.
inline SDL_Rect NarrowToCurrentClip(SDL_Renderer* renderer, const SDL_Rect& clip) {
  if (!SDL_RenderClipEnabled(renderer)) {
    return clip;
  }
  SDL_Rect outer{};
  SDL_GetRenderClipRect(renderer, &outer);
  SDL_Rect narrowed{};
  if (!SDL_GetRectIntersection(&outer, &clip, &narrowed)) {
    return SDL_Rect{outer.x, outer.y, 0, 0};
  }
  return narrowed;
}

class ScopedRenderClip {
 public:
  ScopedRenderClip(SDL_Renderer* renderer, const SDL_Rect& clip) : renderer_(renderer) {
    had_clip_ = SDL_RenderClipEnabled(renderer_);
    if (had_clip_) {
      SDL_GetRenderClipRect(renderer_, &previous_clip_);
    }
    const SDL_Rect narrowed = NarrowToCurrentClip(renderer_, clip);
    SDL_SetRenderClipRect(renderer_, &narrowed);
  }

  ~ScopedRenderClip() {
    if (had_clip_) {
      SDL_SetRenderClipRect(renderer_, &previous_clip_);
    } else {
      SDL_SetRenderClipRect(renderer_, nullptr);
    }
  }

  ScopedRenderClip(const ScopedRenderClip&) = delete;
  ScopedRenderClip& operator=(const ScopedRenderClip&) = delete;

 private:
  SDL_Renderer* renderer_;
  bool had_clip_ = false;
  SDL_Rect previous_clip_{};
};

}  // namespace microide::render
