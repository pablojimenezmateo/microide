// A nested clip narrows the outer one; it never widens it.
//
// A partial repaint runs the whole scene under the dirty rect's clip. A surface
// that set its own clip by REPLACING the renderer's drew outside that rect, over
// pixels that were already right -- harmless for an opaque fill, not for
// anti-aliased text blended over itself, which got bolder each time (the
// editor's "reloaded from disk" banner, found by ProjectChangeRedraw).

#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "render/SurfacePrimitives.h"
#include "render/ScopedRenderClip.h"
#include "support/SoftwareCanvas.h"

#include <string>
#include <cmath>
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

// The same at the scales whose device grid is wider than 8 logical pixels: at
// 1.1x it is 10 (11 device pixels), at 1.65x 20. DevicePixelAlignment looked no
// further than 8 and left such a scale unaligned, and a surface's clip narrowed
// under a partial frame's damage clip then reached one device column past where
// the same surface's clip stops in a full frame -- `ceil(w * s)` jumps from 0 to 2
// device pixels at 1.1x -- so the sidebar painted a pixel of the editor's
// background in partial frames only (soak d213837376ca at ui.scale 1.1).
void TestPartialAndFullFramesClipASurfaceAlikeAtWideGrids() {
  EnsureDummySdlVideoInitialized();
  for (const float scale : {1.1f, 1.65f, 1.3f}) {
    const int device_w = static_cast<int>(std::lround(200.0f * scale));
    const int device_h = static_cast<int>(std::lround(100.0f * scale));
    SoftwareCanvas canvas(device_w, device_h);
    SDL_Renderer* renderer = canvas.renderer();
    Expect(SDL_SetRenderLogicalPresentation(renderer, 200, 100, SDL_LOGICAL_PRESENTATION_STRETCH),
           "the canvas takes a logical presentation");
    for (const int edge : {41, 87, 174 / 2}) {
      const SDL_Rect sidebar{0, 10, edge, 80};
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
      for (int x = 0; x < device_w; ++x) {
        full.push_back(canvas.PixelAt(x, device_h / 2).r == 255);
      }
      const SDL_Rect damage{edge, 20, 100, 50};
      paint(&damage);
      const SDL_Rect aligned = render::AlignClipToDevicePixels(renderer, damage);
      for (int x = 0; x < device_w; ++x) {
        const bool in_damage = static_cast<float>(x) >= std::floor(static_cast<float>(aligned.x) * scale) &&
                               static_cast<float>(x) < static_cast<float>(aligned.x + aligned.w) * scale;
        if (in_damage) {
          Expect((canvas.PixelAt(x, device_h / 2).r == 255) == full[static_cast<std::size_t>(x)],
                 "at " + std::to_string(scale) + "x, a sidebar ending at " + std::to_string(edge) +
                     ": device column " + std::to_string(x) + " is painted as the full frame paints it");
        }
      }
    }
  }
}


// openspec clip-discipline D2: one rule turns a logical rect into whole device
// pixels for clips, fills and damage. Its edges land on device pixels at every
// scale, snapping twice is snapping once, and an empty rect stays empty -- never
// one alignment unit, which would damage and clip k x k pixels for a rect of none.
void TestSnapToDevicePixelsIsWholeIdempotentAndKeepsEmptyEmpty() {
  using microide::render::SnapToDevicePixels;
  const auto whole = [](int logical, float scale) {
    const float device = static_cast<float>(logical) * scale;
    return std::fabs(device - std::round(device)) < 1e-3f;
  };
  const float nan = std::nanf("");
  for (const float scale : {1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 0.75f, 3.0f}) {
    const SDL_FPoint s{scale, scale};
    for (const SDL_FRect rect : {SDL_FRect{10.3f, 7.6f, 33.2f, 18.9f}, SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f},
                                 SDL_FRect{-5.5f, 3.0f, 12.0f, 0.4f}, SDL_FRect{1437.0f, 59.0f, 6.0f, 339.0f}}) {
      const SDL_Rect snapped = SnapToDevicePixels(rect, s);
      const std::string at = " at scale " + std::to_string(scale);
      Expect(whole(snapped.x, scale) && whole(snapped.y, scale) && whole(snapped.x + snapped.w, scale) &&
                 whole(snapped.y + snapped.h, scale),
             "every edge is a whole device pixel" + at);
      Expect(snapped.x <= rect.x && snapped.y <= rect.y &&
                 static_cast<float>(snapped.x + snapped.w) >= rect.x + rect.w &&
                 static_cast<float>(snapped.y + snapped.h) >= rect.y + rect.h,
             "the snapped rect covers the rect" + at);
      const SDL_Rect twice = SnapToDevicePixels(
          SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                    static_cast<float>(snapped.h)},
          s);
      Expect(SameRect(twice, snapped), "snapping twice is snapping once" + at);
    }
    for (const SDL_FRect empty : {SDL_FRect{5.0f, 5.0f, 0.0f, 10.0f}, SDL_FRect{5.0f, 5.0f, 10.0f, -1.0f},
                                  SDL_FRect{nan, 0.0f, 4.0f, 4.0f}, SDL_FRect{0.0f, 0.0f, nan, 4.0f}}) {
      Expect(SameRect(SnapToDevicePixels(empty, s), SDL_Rect{0, 0, 0, 0}),
             "an empty, inverted or NaN rect snaps to nothing at scale " + std::to_string(scale));
    }
  }
  // Soak 0ab3769c6866, the values from its rr trace: a terminal scrollbar's thumb
  // laid out to end where its track ends, at 882.857178 + 53.1428566 -- 936 plus
  // float noise. Its far edge ceiled to 937 and aligned out a whole 4 px step at
  // 1.25x, so the thumb snapped past the track it sits in.
  const SDL_FPoint s{1.25f, 1.25f};
  const SDL_Rect track = SnapToDevicePixels(SDL_FRect{1695.0f, 812.0f, 10.0f, 124.0f}, s);
  const SDL_Rect thumb = SnapToDevicePixels(SDL_FRect{1695.0f, 882.857178f, 10.0f, 53.1428566f}, s);
  Expect(thumb.y + thumb.h <= track.y + track.h,
         "a rect ending on another's edge plus float noise snaps to that edge (thumb ends " +
             std::to_string(thumb.y + thumb.h) + ", track " + std::to_string(track.y + track.h) + ")");
}

// openspec clip-discipline D1/D5: an element paints inside its own scope. A draw
// past its rect is clipped -- a full frame shows what a partial one can -- and,
// while recording, reported by the element's name; a draw inside is not.
void TestAPaintElementClipsAndReportsADrawPastItsRect() {
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas(100, 60);
  SDL_Renderer* renderer = canvas.renderer();
  const bool was = render::PaintOverrunRecordingEnabled();
  render::SetPaintOverrunRecording(true);
  (void)render::TakePaintOverruns();
  {
    const render::PaintElement tab(renderer, SDL_FRect{10.0f, 10.0f, 20.0f, 20.0f}, "tab:0/main.cpp",
                                   SDL_Color{0, 0, 255, 255});
    render::FillRect(renderer, SDL_FRect{12.0f, 12.0f, 10.0f, 10.0f}, SDL_Color{0, 255, 0, 255});
  }
  Expect(render::TakePaintOverruns().empty(), "a draw inside its element is no overrun");
  {
    const render::PaintElement tab(renderer, SDL_FRect{10.0f, 10.0f, 20.0f, 20.0f}, "tab:0/main.cpp");
    render::FillRect(renderer, SDL_FRect{10.0f, 10.0f, 21.0f, 20.0f}, SDL_Color{255, 0, 0, 255});
  }
  SDL_FlushRenderer(renderer);
  const std::vector<render::PaintOverrun> overruns = render::TakePaintOverruns();
  render::SetPaintOverrunRecording(was);
  Expect(overruns.size() == 1 && overruns.front().element == "tab:0/main.cpp" &&
             overruns.front().draw_rect.w == 21 && overruns.front().element_rect.w == 20,
         "a fill one pixel past its element is reported, naming it");
  Expect(canvas.PixelAt(30, 15).r != 255, "and clipped: the pixel past the element is not drawn");
  Expect(canvas.PixelAt(29, 15).r == 255, "while the element's own last column is");
  Expect(canvas.PixelAt(25, 25).b == 255 || canvas.PixelAt(25, 25).r == 255,
         "an opaque element fills its rect first");
}
}  // namespace

void RegisterScopedRenderClipTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ScopedRenderClip/NarrowsToTheOuterClip", TestNarrowsToTheOuterClip);
  AddTest(tests, "ScopedRenderClip/NarrowsInDevicePixelsAtAFractionalScale",
          TestNarrowsInDevicePixelsAtAFractionalScale);
  AddTest(tests, "ScopedRenderClip/PartialAndFullFramesClipASurfaceAlikeAtWideGrids",
          TestPartialAndFullFramesClipASurfaceAlikeAtWideGrids);
  AddTest(tests, "ScopedRenderClip/PartialAndFullFramesClipASurfaceAlikeAt125",
          TestPartialAndFullFramesClipASurfaceAlikeAt125);
  AddTest(tests, "ScopedRenderClip/SnapToDevicePixelsIsWholeIdempotentAndKeepsEmptyEmpty",
          TestSnapToDevicePixelsIsWholeIdempotentAndKeepsEmptyEmpty);
  AddTest(tests, "ScopedRenderClip/APaintElementClipsAndReportsADrawPastItsRect",
          TestAPaintElementClipsAndReportsADrawPastItsRect);
}

}  // namespace microide::tests
