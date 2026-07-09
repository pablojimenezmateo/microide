#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

// Applies `clip` to the renderer for the lifetime of the scope, then restores the
// renderer's PREVIOUS clip state exactly — the enabled rect if one was set, or the
// disabled state if none was.
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
class ScopedRenderClip {
 public:
  ScopedRenderClip(SDL_Renderer* renderer, const SDL_Rect& clip) : renderer_(renderer) {
    had_clip_ = SDL_RenderClipEnabled(renderer_);
    if (had_clip_) {
      SDL_GetRenderClipRect(renderer_, &previous_clip_);
    }
    SDL_SetRenderClipRect(renderer_, &clip);
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
