#include "render/PaintElement.h"

#include <algorithm>
#include <cstdlib>

#include "render/DevicePixelSnap.h"
#include "render/SurfacePrimitives.h"

namespace microide::render {
namespace {

bool InitialRecording() {
  const char* value = std::getenv("MICROIDE_QA_VERIFY_REDRAW");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool& RecordingFlag() {
  static bool enabled = InitialRecording();
  return enabled;
}

// The innermost open scope and the overruns noted, per painting thread (paint is
// single-threaded; the thread_local keeps tests in parallel shards apart).
thread_local PaintElement* t_innermost = nullptr;
thread_local PaintPassScope* t_pass = nullptr;
thread_local std::vector<PaintOverrun> t_overruns;

bool Contains(const SDL_Rect& outer, const SDL_Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

}  // namespace

bool PaintOverrunRecordingEnabled() { return RecordingFlag(); }

void SetPaintOverrunRecording(bool enabled) { RecordingFlag() = enabled; }

void NotePaint(SDL_Renderer* renderer, const SDL_FRect& logical) {
  if (!RecordingFlag()) {
    return;
  }
  if (t_pass != nullptr) {
    t_pass->Check(renderer, logical);
  }
  if (t_innermost == nullptr) {
    return;
  }
  const SDL_Rect drawn = SnapToDevicePixels(renderer, logical);
  if (drawn.w == 0 || drawn.h == 0 || Contains(t_innermost->snapped(), drawn)) {
    return;
  }
  const std::string_view name = t_innermost->name();
  if (std::none_of(t_overruns.begin(), t_overruns.end(),
                   [name](const PaintOverrun& seen) { return seen.element == name; })) {
    t_overruns.push_back(PaintOverrun{std::string(name), t_innermost->snapped(), drawn, t_innermost->logical(), logical,
                                      device_pixel_snap_detail::DeviceScale(renderer)});
  }
}

std::vector<PaintOverrun> TakePaintOverruns() {
  std::vector<PaintOverrun> taken;
  taken.swap(t_overruns);
  return taken;
}

PaintPassScope::PaintPassScope(SDL_Renderer* renderer, std::string_view name, Covers covers, const void* context)
    : covers_(covers), context_(context), parent_(t_pass) {
  (void)renderer;
  if (RecordingFlag()) {
    name_ = "pass:";
    name_ += name;
    t_pass = this;
  }
}

PaintPassScope::~PaintPassScope() {
  if (t_pass == this) {
    t_pass = parent_;
  }
}

void PaintPassScope::Check(SDL_Renderer* renderer, const SDL_FRect& logical) const {
  // What of the draw can show: inside the innermost element (it clips to its
  // rect) and inside the renderer's clip (a pane's own clip, a partial frame's).
  SDL_FRect visible = logical;
  const auto intersect = [&visible](const SDL_FRect& with) {
    SDL_FRect both{};
    if (!SDL_GetRectIntersectionFloat(&visible, &with, &both)) {
      return false;
    }
    visible = both;
    return true;
  };
  if (t_innermost != nullptr && !intersect(t_innermost->logical())) {
    return;
  }
  if (SDL_RenderClipEnabled(renderer)) {
    SDL_Rect clip{};
    if (SDL_GetRenderClipRect(renderer, &clip) &&
        !intersect(SDL_FRect{static_cast<float>(clip.x), static_cast<float>(clip.y), static_cast<float>(clip.w),
                             static_cast<float>(clip.h)})) {
      return;
    }
  }
  if (visible.w <= 0.0f || visible.h <= 0.0f || covers_(context_, visible)) {
    return;
  }
  if (std::none_of(t_overruns.begin(), t_overruns.end(),
                   [this](const PaintOverrun& seen) { return seen.element == name_; })) {
    t_overruns.push_back(PaintOverrun{name_, SDL_Rect{}, SnapToDevicePixels(renderer, visible), SDL_FRect{}, visible,
                                      device_pixel_snap_detail::DeviceScale(renderer)});
  }
}

PaintElement::PaintElement(SDL_Renderer* renderer, const SDL_FRect& rect, std::string_view name,
                           std::optional<SDL_Color> opaque_background)
    : logical_(rect), snapped_(SnapToDevicePixels(renderer, rect)), name_(name), parent_(t_innermost) {
  clip_.emplace(renderer, snapped_);
  t_innermost = this;
  if (opaque_background.has_value()) {
    // The element owns its background: a partial frame cannot leave older paint
    // under what it draws (clip-discipline cause 4).
    FillRect(renderer, rect, *opaque_background);
  }
}

PaintElement::~PaintElement() {
  t_innermost = parent_;
  clip_.reset();
}

}  // namespace microide::render
