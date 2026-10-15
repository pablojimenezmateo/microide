#include "app/RedrawVerifier.h"

#include <cstdio>

#include "util/PerformanceTrace.h"

namespace microide::app {

namespace {

// Both readbacks come from targets of one format (the scene and reference
// textures are created alike), so the bytewise diff needs no conversion; this only
// guards the assumption.
bool Diffable(const SDL_Surface* surface) {
  return surface != nullptr && SDL_BYTESPERPIXEL(surface->format) == 4 && !SDL_MUSTLOCK(surface);
}

render::PixelImageView View(const SDL_Surface* surface) {
  return render::PixelImageView{static_cast<const std::uint8_t*>(surface->pixels), surface->w,
                                surface->h, surface->pitch};
}

}  // namespace

std::string FormatStaleFrameReport(const render::PixelDiffResult& diff,
                                   const VerifiedFrameInfo& frame) {
  char head[256];
  std::snprintf(head, sizeof(head),
                "QA-STALE frame=%llu reason=%s pixels=%zu bbox=%d,%d,%d,%d first=%d,%d clips=",
                static_cast<unsigned long long>(frame.frame_index),
                frame.reason != nullptr ? frame.reason : "?", diff.count, diff.min_x, diff.min_y,
                diff.box_width, diff.box_height, diff.first_x, diff.first_y);
  std::string report(head);
  bool first = true;
  for (const SDL_Rect& clip : frame.clips) {
    char rect[64];
    std::snprintf(rect, sizeof(rect), "%s%d,%d,%d,%d", first ? "" : ";", clip.x, clip.y, clip.w,
                  clip.h);
    report += rect;
    first = false;
  }
  if (first) {
    report += "none";
  }
  return report;
}

bool RedrawVerifier::EnabledFromEnvironment() {
  return util::PerformanceTrace::FlagEnabled("MICROIDE_QA_VERIFY_REDRAW");
}

void RedrawVerifier::Configure(bool enabled) {
  enabled_ = enabled;
  if (!enabled_) {
    Destroy();
  }
}

bool RedrawVerifier::BindReference(SDL_Renderer* renderer, int logical_width, int logical_height) {
  if (!reference_.Ensure(renderer, logical_width, logical_height) ||
      !SDL_SetRenderTarget(renderer, reference_.texture())) {
    return false;
  }
  reference_.ApplyRenderTargetPresentation(renderer);
  SDL_SetRenderClipRect(renderer, nullptr);
  return true;
}

std::optional<render::PixelDiffResult> RedrawVerifier::CompareWithReference(
    SDL_Renderer* renderer,
    SDL_Texture* scene,
    SDL_Surface* retained,
    const VerifiedFrameInfo& frame) {
  SDL_SetRenderClipRect(renderer, nullptr);
  SDL_Surface* reference = SDL_RenderReadPixels(renderer, nullptr);
  SDL_SetRenderTarget(renderer, scene);
  std::optional<render::PixelDiffResult> result;
  if (Diffable(retained) && Diffable(reference) && retained->format == reference->format) {
    result = render::DiffPixels(View(retained), View(reference));
    ++verified_frame_count_;
    if (!result->identical()) {
      ++stale_frame_count_;
      last_report_ = FormatStaleFrameReport(*result, frame);
      SDL_Log("%s", last_report_.c_str());
    }
  }
  SDL_DestroySurface(reference);
  SDL_DestroySurface(retained);
  return result;
}

}  // namespace microide::app
