#include "app/SceneTexturePresenter.h"

namespace microide::app {

bool SceneTexturePresenter::Ensure(SDL_Renderer* renderer, int logical_width, int logical_height) {
  if (renderer == nullptr || logical_width <= 0 || logical_height <= 0) {
    return false;
  }

  // The renderer's PIXEL output, which is what the texture is sized in. It is not
  // the logical size whenever the display scale or the UI scale is anything but
  // 1.0 -- and this used to compare the two and give up when they differed, which
  // disabled the retained scene (and with it every partial frame) outright on any
  // HiDPI display. See the header.
  int output_width = 0;
  int output_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height) || output_width <= 0 ||
      output_height <= 0) {
    Destroy();
    return false;
  }

  if (texture_ != nullptr && pixel_width_ == output_width && pixel_height_ == output_height) {
    if (logical_width_ != logical_width || logical_height_ != logical_height) {
      // Same drawable, different logical grid (a UI-scale change). The texture is
      // still the right size in pixels, so re-map rather than reallocate -- but
      // what it holds was laid out for the old grid and cannot be re-presented.
      logical_width_ = logical_width;
      logical_height_ = logical_height;
      presentation_applied_ = false;
      valid_ = false;
    }
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
                               output_width, output_height);
  if (texture_ == nullptr) {
    return false;
  }

  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
  // The retained scene is an opaque full-window backbuffer: nothing is behind it,
  // and the window is never cleared between presents. SDL defaults an RGBA target
  // to SDL_BLENDMODE_BLEND, which made every present composite the scene against
  // the *previous* present instead of replacing it — so any region whose alpha was
  // not 255 darkened a little more each frame until it went flat (that is what ate
  // the editor behind a modal, TD-2026-07-30-100). NONE is also the cheaper blit.
  SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
  pixel_width_ = output_width;
  pixel_height_ = output_height;
  logical_width_ = logical_width;
  logical_height_ = logical_height;
  presentation_applied_ = false;
  valid_ = false;
  return true;
}

void SceneTexturePresenter::ApplyRenderTargetPresentation(SDL_Renderer* renderer) {
  if (renderer == nullptr || texture_ == nullptr || presentation_applied_) {
    return;
  }
  // A render target carries its OWN logical presentation, which starts disabled
  // and survives being unbound and rebound (so this runs once per texture, not
  // once per frame). Giving the texture view the window view's presentation is
  // what makes the shell's logical-coordinate drawing land on the texture's
  // device pixels exactly as it would have landed on the window's -- and what
  // makes re-presenting the texture over the full logical rect a 1:1 texel blit
  // rather than a resample.
  if (!SDL_SetRenderLogicalPresentation(renderer, logical_width_, logical_height_,
                                        SDL_LOGICAL_PRESENTATION_STRETCH)) {
    SDL_Log("SDL_SetRenderLogicalPresentation(scene texture) failed: %s", SDL_GetError());
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
  logical_width_ = 0;
  logical_height_ = 0;
  presentation_applied_ = false;
  valid_ = false;
}

}  // namespace microide::app
