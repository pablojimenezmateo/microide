#include "app/SceneTexturePresenter.h"

namespace microide::app {

bool SceneTexturePresenter::Ensure(SDL_Renderer* renderer, const RenderPresentation& presentation) {
  if (renderer == nullptr || presentation.logical_width <= 0 || presentation.logical_height <= 0) {
    return false;
  }

  int output_width = 0;
  int output_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height) || output_width <= 0 ||
      output_height <= 0) {
    Destroy();
    return false;
  }

  if (texture_ != nullptr && pixel_width_ == output_width && pixel_height_ == output_height) {
    if (presentation_ != presentation) {
      presentation_ = presentation;
      presentation_applied_ = false;
      valid_ = false;
    }
    return true;
  }

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
                               output_width, output_height);
  if (texture_ == nullptr) {
    return false;
  }

  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
  SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
  pixel_width_ = output_width;
  pixel_height_ = output_height;
  presentation_ = presentation;
  presentation_applied_ = false;
  valid_ = false;
  return true;
}

void SceneTexturePresenter::ApplyRenderTargetPresentation(SDL_Renderer* renderer) {
  if (renderer == nullptr || texture_ == nullptr || presentation_applied_) {
    return;
  }
  if (!ApplyRenderPresentation(renderer, presentation_)) {
    SDL_Log("ApplyRenderPresentation(scene texture) failed: %s", SDL_GetError());
    return;
  }
  presentation_applied_ = true;
}

void SceneTexturePresenter::Destroy() {
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  pixel_width_ = 0;
  pixel_height_ = 0;
  presentation_ = RenderPresentation{};
  presentation_applied_ = false;
  valid_ = false;
}

}
