#include "TestSupport.h"

#include "render/TextRenderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests {

struct TextRendererTestAccess {
  static void SetBackend(microide::render::TextRenderer& renderer,
                         std::unique_ptr<microide::render::TextRendererBackend> backend) {
    renderer.backend_ = std::move(backend);
    renderer.attempted_optional_backend_ = true;
    renderer.ClearWidthCache();
    renderer.width_cache_initialized_ = false;
  }
};

namespace {

class CountingTextBackend final : public microide::render::TextRendererBackend {
 public:
  const char* Name() const override { return "counting"; }

  void SetPresentationScale(float scale_x, float scale_y) override {
    scale_x_ = scale_x;
    scale_y_ = scale_y;
  }

  float CharWidth() const override { return scale_x_; }
  float LineHeight() const override { return 14.0f * scale_y_; }

  float MeasureWidth(std::string_view text) const override {
    ++measure_width_calls_;
    return static_cast<float>(text.size()) * scale_x_;
  }

  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override {
    (void) renderer;
    (void) x;
    (void) y;
    (void) color;
    (void) text;
  }

  int measure_width_calls() const { return measure_width_calls_; }

 private:
  mutable int measure_width_calls_ = 0;
  float scale_x_ = 1.0f;
  float scale_y_ = 1.0f;
};

#if MICROIDE_HAS_SDL3_TTF

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}

class SoftwareCanvas final {
 public:
  SoftwareCanvas(int width, int height) {
    surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    Expect(surface_ != nullptr, "renderer regression test should allocate a software surface");
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    Expect(renderer_ != nullptr, "renderer regression test should create a software renderer");
  }

  ~SoftwareCanvas() {
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (surface_ != nullptr) {
      SDL_DestroySurface(surface_);
    }
  }

  SDL_Renderer* renderer() const { return renderer_; }

 private:
  SDL_Surface* surface_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
};

bool IsRedDominant(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return a > 0 && r >= 24 && r > g + 12 && r > b + 12;
}

bool IsGreenDominant(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return a > 0 && g >= 24 && g > r + 12 && g > b + 12;
}

void TestSdlTtfAsciiGlyphsStayWithinLineBands() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(320, 160);

  microide::render::TextRenderer renderer;
  renderer.EnsureInitialized(canvas.renderer());
  Expect(renderer.BackendName() == "sdl3_ttf",
         "SDL_ttf regression test should exercise the real font backend");

  SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255);
  Expect(SDL_RenderClear(canvas.renderer()),
         "renderer regression test should clear the software canvas");

  const float line_height = std::max(1.0f, renderer.LineHeight());
  const float first_line_y = 6.0f;
  const float second_line_y = first_line_y + line_height;
  renderer.DrawString(canvas.renderer(), 8.0f, first_line_y, SDL_Color{255, 0, 0, 255}, "HELLO");
  renderer.DrawString(canvas.renderer(), 8.0f, second_line_y, SDL_Color{0, 255, 0, 255}, "WORLD");

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "renderer regression test should read back rendered pixels");

  bool found_red = false;
  bool found_green = false;
  int max_red_y = -1;
  int min_green_y = pixels->h;
  for (int y = 0; y < pixels->h; ++y) {
    for (int x = 0; x < pixels->w; ++x) {
      Uint8 r = 0;
      Uint8 g = 0;
      Uint8 b = 0;
      Uint8 a = 0;
      Expect(SDL_ReadSurfacePixel(pixels, x, y, &r, &g, &b, &a),
             "renderer regression test should read software pixels");
      if (IsRedDominant(r, g, b, a)) {
        found_red = true;
        max_red_y = std::max(max_red_y, y);
      }
      if (IsGreenDominant(r, g, b, a)) {
        found_green = true;
        min_green_y = std::min(min_green_y, y);
      }
    }
  }

  SDL_DestroySurface(pixels);

  Expect(found_red, "first ASCII line should render red pixels");
  Expect(found_green, "second ASCII line should render green pixels");
  Expect(max_red_y < static_cast<int>(std::floor(second_line_y)),
         "ASCII glyph rendering should stay inside its line band instead of spilling into the next row");
  Expect(min_green_y >= static_cast<int>(std::floor(second_line_y)),
         "the second ASCII line should begin at its own row band");
}

#endif

void TestMeasureWidthCachesRepeatedStrings() {
  microide::render::TextRenderer renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(renderer, std::move(backend));

  const float first = renderer.MeasureWidth("status");
  const float second = renderer.MeasureWidth("status");

  Expect(first == second, "text renderer should return the cached width for repeated labels");
  Expect(backend_ptr->measure_width_calls() == 1,
         "text renderer should only ask the backend once for the same string");
}

void TestMeasureWidthCacheClearsWhenPresentationScaleChanges() {
  microide::render::TextRenderer renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(renderer, std::move(backend));

  renderer.EnsureInitialized(nullptr, 1.0f, 1.0f);
  const float base_width = renderer.MeasureWidth("sidebar");
  renderer.EnsureInitialized(nullptr, 2.0f, 1.0f);
  const float scaled_width = renderer.MeasureWidth("sidebar");

  Expect(base_width == 7.0f, "baseline width should use the initial presentation scale");
  Expect(scaled_width == 14.0f, "cache should invalidate when presentation scale changes");
  Expect(backend_ptr->measure_width_calls() == 2,
         "scale changes should force a fresh backend measurement");
}

void TestTruncateToWidthUsesUtf8BoundariesAndFewMeasurements() {
  microide::render::TextRenderer renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(renderer, std::move(backend));

  const std::string truncated = renderer.TruncateToWidth("abcdefghijklmnop", 10.0f);

  Expect(truncated == "abcdefg...",
         "truncate-to-width should keep the longest fitting prefix plus ellipsis");
  Expect(backend_ptr->measure_width_calls() <= 8,
         "truncate-to-width should avoid probing every prefix width linearly");
}

}  // namespace

void RegisterTextRendererTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextRenderer caches repeated width lookups", TestMeasureWidthCachesRepeatedStrings);
  AddTest(tests,
          "TextRenderer invalidates width cache on scale changes",
          TestMeasureWidthCacheClearsWhenPresentationScaleChanges);
  AddTest(tests,
          "TextRenderer truncation uses bounded width probes",
          TestTruncateToWidthUsesUtf8BoundariesAndFewMeasurements);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests,
          "TextRenderer SDL_ttf ASCII glyphs stay within line bands",
          TestSdlTtfAsciiGlyphsStayWithinLineBands);
#endif
}

}  // namespace microide::tests
