// A nested clip narrows the outer one; it never widens it.
//
// A partial repaint runs the whole scene under the dirty rect's clip. A surface
// that set its own clip by REPLACING the renderer's drew outside that rect, over
// pixels that were already right -- harmless for an opaque fill, not for
// anti-aliased text blended over itself, which got bolder each time (the
// editor's "reloaded from disk" banner, found by ProjectChangeRedraw).

#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "render/ScopedRenderClip.h"
#include "support/SoftwareCanvas.h"

#include <vector>

namespace microide::tests {
namespace {

bool SameRect(const SDL_Rect& lhs, const SDL_Rect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

SDL_Rect CurrentClip(SDL_Renderer* renderer) {
  SDL_Rect clip{};
  SDL_GetRenderClipRect(renderer, &clip);
  return clip;
}

void TestNarrowsToTheOuterClip() {
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas(200, 100);
  SDL_Renderer* renderer = canvas.renderer();

  const SDL_Rect outer{10, 10, 50, 50};
  SDL_SetRenderClipRect(renderer, &outer);
  {
    const render::ScopedRenderClip inner(renderer, SDL_Rect{40, 0, 100, 30});
    Expect(SameRect(CurrentClip(renderer), SDL_Rect{40, 10, 20, 20}),
           "a nested clip is the intersection with the outer one");
  }
  Expect(SameRect(CurrentClip(renderer), outer), "leaving the scope restores the outer clip");

  {
    const render::ScopedRenderClip disjoint(renderer, SDL_Rect{120, 70, 30, 20});
    Expect(SDL_RenderClipEnabled(renderer) && CurrentClip(renderer).w == 0,
           "a nested clip outside the outer one clips everything, not nothing");
    // And it draws nothing: the whole point.
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    const SDL_FRect fill{0.0f, 0.0f, 200.0f, 100.0f};
    SDL_RenderFillRect(renderer, &fill);
  }
  const SDL_Color pixel = canvas.PixelAt(130, 75);
  Expect(pixel.r != 255, "a draw under a disjoint nested clip must not reach the canvas");

  SDL_SetRenderClipRect(renderer, nullptr);
  {
    const render::ScopedRenderClip only(renderer, SDL_Rect{5, 6, 7, 8});
    Expect(SameRect(CurrentClip(renderer), SDL_Rect{5, 6, 7, 8}),
           "with no outer clip the scope's own clip applies as given");
  }
  Expect(!SDL_RenderClipEnabled(renderer), "and leaving it restores no clip at all");
}

}  // namespace

void RegisterScopedRenderClipTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ScopedRenderClip/NarrowsToTheOuterClip", TestNarrowsToTheOuterClip);
}

}  // namespace microide::tests
