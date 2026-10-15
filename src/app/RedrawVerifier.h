#pragma once

// QA redraw verifier: is the frame the user sees the frame the state describes?
//
// A partial frame repaints only its dirty clips over the retained scene, so its
// correctness rests on every damage site naming the right rects and every
// RenderClip skip being right -- and when either is wrong the result is stale
// pixels that no unit test sees unless its author thought of that case (an editor
// painted black under the Edit menu, AA text drawn over itself on partial frames).
// With MICROIDE_QA_VERIFY_REDRAW=1 at startup, after every PARTIAL frame the app
// renders the same prepared frame in full into a second offscreen target, reads
// both back and diffs them (render/PixelDiff); any difference is one `QA-STALE`
// log line naming the differing region, the frame's clips and its redraw reason.
//
// It reports and never repairs: the retained scene stays exactly what the partial
// render produced, so a finding can also be seen and screenshotted. Off (the
// default), it allocates nothing and the only cost is one flag check per frame.
//
// The second render runs after the partial frame was produced, so it cannot
// change what was compared; render-path side effects it triggers (hover refresh,
// signature-help expiry, text-input area updates) can only affect the NEXT frame,
// and only in QA runs.

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "app/SceneTexturePresenter.h"
#include "render/PixelDiff.h"

namespace microide::app {

struct VerifiedFrameInfo {
  std::span<const SDL_Rect> clips;
  const char* reason = nullptr;
  std::uint64_t frame_index = 0;
};

// The `QA-STALE` line for one stale frame. Pure, so its shape is pinned by a test:
// drivers grep for the prefix and parse the fields.
std::string FormatStaleFrameReport(const render::PixelDiffResult& diff,
                                   const VerifiedFrameInfo& frame);

class RedrawVerifier {
 public:
  RedrawVerifier() = default;
  ~RedrawVerifier() { Destroy(); }

  RedrawVerifier(const RedrawVerifier&) = delete;
  RedrawVerifier& operator=(const RedrawVerifier&) = delete;

  // MICROIDE_QA_VERIFY_REDRAW=1, read once at startup.
  static bool EnabledFromEnvironment();

  void Configure(bool enabled);
  // Where to save the two images of a stale frame (retained and reference, as
  // BMP), for the first kMaxDumpedFrames of them: a finding's bounding box says
  // where, the images say what. MICROIDE_QA_STALE_DUMP_DIR, read at startup.
  void SetDumpDirectory(std::string directory) { dump_directory_ = std::move(directory); }
  static constexpr std::size_t kMaxDumpedFrames = 8;
  bool enabled() const { return enabled_; }

  // Compare the retained scene (the bound render target, clip cleared) with a full
  // render of the same frame. `render_full` paints the frame with no clip and no
  // hint into whatever target is bound. Restores `scene` as the render target
  // before returning. Returns the diff when a comparison ran, nullopt when it could
  // not (verifier off, reference target unavailable during a resize, readback
  // failure). A stale frame is logged and counted here.
  template <typename RenderFull>
  std::optional<render::PixelDiffResult> Verify(SDL_Renderer* renderer,
                                                SDL_Texture* scene,
                                                int logical_width,
                                                int logical_height,
                                                const VerifiedFrameInfo& frame,
                                                RenderFull&& render_full) {
    if (!enabled_ || renderer == nullptr || scene == nullptr) {
      return std::nullopt;
    }
    SDL_Surface* retained = SDL_RenderReadPixels(renderer, nullptr);
    if (retained == nullptr || !BindReference(renderer, logical_width, logical_height)) {
      SDL_DestroySurface(retained);
      SDL_SetRenderTarget(renderer, scene);
      return std::nullopt;
    }
    std::forward<RenderFull>(render_full)();
    return CompareWithReference(renderer, scene, retained, frame);
  }

  // Release the reference target. Must run before the renderer is destroyed.
  void Destroy() { reference_.Destroy(); }

  std::size_t stale_frame_count() const { return stale_frame_count_; }
  std::size_t verified_frame_count() const { return verified_frame_count_; }
  const std::string& last_report() const { return last_report_; }

 private:
  bool BindReference(SDL_Renderer* renderer, int logical_width, int logical_height);
  // Reads the reference back, diffs it against `retained` (consumed), rebinds
  // `scene`, and logs a stale frame.
  std::optional<render::PixelDiffResult> CompareWithReference(SDL_Renderer* renderer,
                                                              SDL_Texture* scene,
                                                              SDL_Surface* retained,
                                                              const VerifiedFrameInfo& frame);

  bool enabled_ = false;
  // The second offscreen target: same pixel size and logical presentation as the
  // scene texture, which is exactly what SceneTexturePresenter manages.
  SceneTexturePresenter reference_;
  std::size_t stale_frame_count_ = 0;
  std::size_t verified_frame_count_ = 0;
  std::string last_report_;
  std::string dump_directory_;
};

}  // namespace microide::app
