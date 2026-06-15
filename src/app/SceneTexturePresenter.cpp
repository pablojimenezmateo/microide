#include "app/SceneTexturePresenter.h"

namespace microide::app {

bool SceneTexturePresenter::Ensure(SDL_Renderer* renderer, int logical_width, int logical_height) {
  if (renderer == nullptr || logical_width <= 0 || logical_height <= 0) {
    return false;
  }

  int output_width = 0;
  int output_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height) || output_width <= 0 ||
      output_height <= 0) {
    Destroy();
    return false;
  }
  if (output_width != logical_width || output_height != logical_height) {
    Destroy();
    return false;
  }

  if (texture_ != nullptr && width_ == logical_width && height_ == logical_height) {
    return true;
  }

  // Coalesce: while the user is actively dragging the window we'd otherwise
  // destroy and recreate the full-window render target on every resize event.
  // Hold off on the realloc and let this frame fall through to the
  // fallback-full path (which renders directly to the window). Once
  // kResizeSettleNs elapses without a resize event, the texture is rebuilt once.
  if (last_resize_event_ns_ != 0) {
    const Uint64 now_ns = SDL_GetTicksNS();
    if (now_ns - last_resize_event_ns_ < kResizeSettleNs) {
      Destroy();
      return false;
    }
    last_resize_event_ns_ = 0;
  }

  Destroy();
  texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                               logical_width, logical_height);
  if (texture_ == nullptr) {
    return false;
  }

  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
  width_ = logical_width;
  height_ = logical_height;
  valid_ = false;
  return true;
}

void SceneTexturePresenter::Destroy() {
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
  valid_ = false;
}

}  // namespace microide::app
