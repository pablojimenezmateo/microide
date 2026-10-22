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

#include <string>
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

// At a fractional display scale a clip is device pixels SDL rounds out of the
// logical rect -- [floor(y*s), floor(y*s) + ceil(h*s)) -- so a nested clip that
// is the LOGICAL intersection is not the device intersection. Two ways it went
// wrong at 1.5x (CI's qa lane, scaled): the outer clip's bottom half-pixel row
// was left out, so the sidebar's focus ring kept a gap the partial frame never
// repainted; and a surface whose top edge shares a half pixel with the outer
// clip's bottom (the tab strip under the menu bar) got an EMPTY clip though the
// outer one covered a row it paints, so the menu bar's colour stayed there.
void TestNarrowsInDevicePixelsAtAFractionalScale() {
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas(300, 150);
  SDL_Renderer* renderer = canvas.renderer();
  Expect(SDL_SetRenderLogicalPresentation(renderer, 200, 100, SDL_LOGICAL_PRESENTATION_STRETCH),
         "the canvas takes a 1.5x logical presentation");
  const auto fill_red_under = [&](const SDL_Rect& outer, const SDL_Rect& inner) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_RenderClear(renderer);
    SDL_SetRenderClipRect(renderer, &outer);
    {
      const render::ScopedRenderClip scope(renderer, inner);
      SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
      const SDL_FRect all{0.0f, 0.0f, 200.0f, 100.0f};
      SDL_RenderFillRect(renderer, &all);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
  };
  const auto red = [&](int x, int y) { return canvas.PixelAt(x, y).r == 255; };

  // Outer: logical rows 0..23 -> device rows 0..34 (34.5 rounds up). Inner from
  // logical 11 -> device 16 (16.5 rounds down).
  fill_red_under(SDL_Rect{0, 0, 200, 23}, SDL_Rect{0, 11, 50, 80});
  Expect(red(10, 34), "the outer clip's last device row is painted under the nested clip");
  Expect(red(10, 16), "and the nested clip's first");
  Expect(!red(10, 35), "nothing past the outer clip (device row 35 on)");
  Expect(!red(10, 12), "nothing far above the nested clip");
  Expect(!red(80, 20), "nothing right of the nested clip (device column 75 on)");

  // Outer: logical rows 0..11 -> device rows 0..16; inner from logical 11 ->
  // device 16. Logically disjoint, they share device row 16.
  fill_red_under(SDL_Rect{0, 0, 200, 11}, SDL_Rect{0, 11, 50, 80});
  Expect(red(10, 16), "the device row the two clips share is painted");
  Expect(!red(10, 17), "and nothing past the outer clip");

  // The outer clip starts inside the surface and runs past its end (CI's qa,
  // menu-clicks: a tab strip under an open submenu). Nothing may be painted above
  // the outer clip's first device row -- a fallback to the surface's own start
  // painted the tab over the submenu above.
  fill_red_under(SDL_Rect{0, 46, 200, 28}, SDL_Rect{0, 27, 50, 30});
  Expect(!red(10, 68), "nothing above the outer clip (device row 69 on)");
  Expect(red(10, 69) && red(10, 84), "and the rows the two share are painted");

  // Really disjoint stays a zero-area clip.
  fill_red_under(SDL_Rect{0, 0, 200, 10}, SDL_Rect{0, 40, 50, 20});
  Expect(!red(10, 61) && !red(10, 5), "clips with no device row in common draw nothing");
}

// TD-2026-10-22-376, the QA monkey's seed 3 at 1.25x: a sidebar ending at
// logical 41 (device 51.25) under a partial frame's damage clip starting at 41.
// Whatever the narrowing chose, SDL's separate rounding of start and length left
// the partial frame one device column off the full one at the sidebar's edge.
// With every clip grown to whole device pixels, a surface paints the same device
// pixels inside the damage in both frames.
void TestPartialAndFullFramesClipASurfaceAlikeAt125() {
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas(250, 125);
  SDL_Renderer* renderer = canvas.renderer();
  Expect(SDL_SetRenderLogicalPresentation(renderer, 200, 100, SDL_LOGICAL_PRESENTATION_STRETCH),
         "the canvas takes a 1.25x logical presentation");
  const SDL_Rect sidebar{0, 10, 41, 80};
  const auto paint = [&](const SDL_Rect* damage) {
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (damage != nullptr) {
      const SDL_Rect aligned = render::AlignClipToDevicePixels(renderer, *damage);
      SDL_SetRenderClipRect(renderer, &aligned);
    }
    {
      const render::ScopedRenderClip scope(renderer, sidebar);
      SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
      const SDL_FRect all{0.0f, 0.0f, 200.0f, 100.0f};
      SDL_RenderFillRect(renderer, &all);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
  };
  paint(nullptr);
  std::vector<bool> full;
  for (int x = 40; x < 70; ++x) {
    full.push_back(canvas.PixelAt(x, 60).r == 255);
  }
  const SDL_Rect damage{41, 20, 100, 50};
  paint(&damage);
  const SDL_Rect aligned = render::AlignClipToDevicePixels(renderer, damage);
  for (int x = 40; x < 70; ++x) {
    const bool in_damage = static_cast<float>(x) >= static_cast<float>(aligned.x) * 1.25f &&
                           static_cast<float>(x) < static_cast<float>(aligned.x + aligned.w) * 1.25f;
    if (in_damage) {
      Expect((canvas.PixelAt(x, 60).r == 255) == full[static_cast<std::size_t>(x - 40)],
             "device column " + std::to_string(x) + " is painted as the full frame paints it");
    }
  }
}

}  // namespace

void RegisterScopedRenderClipTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ScopedRenderClip/NarrowsToTheOuterClip", TestNarrowsToTheOuterClip);
  AddTest(tests, "ScopedRenderClip/NarrowsInDevicePixelsAtAFractionalScale",
          TestNarrowsInDevicePixelsAtAFractionalScale);
  AddTest(tests, "ScopedRenderClip/PartialAndFullFramesClipASurfaceAlikeAt125",
          TestPartialAndFullFramesClipASurfaceAlikeAt125);
}

}  // namespace microide::tests
