#pragma once

#include <SDL3/SDL.h>

#include "app/RenderPresentation.h"

namespace microide::app {

class SceneTexturePresenter {
 public:
  SceneTexturePresenter() = default;
  ~SceneTexturePresenter() { Destroy(); }

  SceneTexturePresenter(const SceneTexturePresenter&) = delete;
  SceneTexturePresenter& operator=(const SceneTexturePresenter&) = delete;

  bool Ensure(SDL_Renderer* renderer, const RenderPresentation& presentation);

  void ApplyRenderTargetPresentation(SDL_Renderer* renderer);

  void Destroy();

  void NoteResizeEvent(Uint64 now_ns) { last_resize_event_ns_ = now_ns; }

  SDL_Texture* texture() const { return texture_; }
  bool valid() const { return valid_; }
  void MarkValid() { valid_ = true; }
  void Invalidate() { valid_ = false; }

  int pixel_width() const { return pixel_width_; }
  int pixel_height() const { return pixel_height_; }

 private:
  static constexpr Uint64 kResizeSettleNs = 150ULL * 1'000'000ULL;

  SDL_Texture* texture_ = nullptr;
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  RenderPresentation presentation_{};
  bool presentation_applied_ = false;
  bool valid_ = false;
  Uint64 last_resize_event_ns_ = 0;
};

}
