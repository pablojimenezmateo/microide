#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "app/SceneTexturePresenter.h"
#include "render/PixelDiff.h"
#include "render/SdlSurface.h"

namespace microide::app {

struct VerifiedFrameInfo {
  std::span<const SDL_Rect> clips;
  const char* reason = nullptr;
  std::uint64_t frame_index = 0;
  float scale = 1.0f;
};

std::string FormatStaleFrameReport(const render::PixelDiffResult& diff,
                                   const VerifiedFrameInfo& frame);

class RedrawVerifier {
 public:
  RedrawVerifier() = default;
  ~RedrawVerifier() { Destroy(); }

  RedrawVerifier(const RedrawVerifier&) = delete;
  RedrawVerifier& operator=(const RedrawVerifier&) = delete;

  static bool EnabledFromEnvironment();

  void Configure(bool enabled);
  void SetDumpDirectory(std::string directory) { dump_directory_ = std::move(directory); }
  static constexpr std::size_t kMaxDumpedFrames = 8;
  bool enabled() const { return enabled_; }

  template <typename RenderFull>
  std::optional<render::PixelDiffResult> Verify(SDL_Renderer* renderer,
                                                SDL_Texture* scene,
                                                const RenderPresentation& presentation,
                                                const VerifiedFrameInfo& frame,
                                                RenderFull&& render_full) {
    if (!enabled_ || renderer == nullptr || scene == nullptr) {
      return std::nullopt;
    }
    render::SurfacePtr retained(SDL_RenderReadPixels(renderer, nullptr));
    if (retained == nullptr || !BindReference(renderer, presentation)) {
      SDL_SetRenderTarget(renderer, scene);
      return std::nullopt;
    }
    std::forward<RenderFull>(render_full)();
    return CompareWithReference(renderer, scene, retained.get(), frame);
  }

  void Destroy() { reference_.Destroy(); }

  std::size_t stale_frame_count() const { return stale_frame_count_; }
  std::size_t verified_frame_count() const { return verified_frame_count_; }
  const std::string& last_report() const { return last_report_; }

 private:
  bool BindReference(SDL_Renderer* renderer, const RenderPresentation& presentation);
  std::optional<render::PixelDiffResult> CompareWithReference(SDL_Renderer* renderer,
                                                              SDL_Texture* scene,
                                                              SDL_Surface* retained,
                                                              const VerifiedFrameInfo& frame);

  bool enabled_ = false;
  SceneTexturePresenter reference_;
  std::size_t stale_frame_count_ = 0;
  std::size_t verified_frame_count_ = 0;
  std::string last_report_;
  std::string dump_directory_;
};

}
