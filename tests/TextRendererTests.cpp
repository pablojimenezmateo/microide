#include "TestSupport.h"

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if MICROIDE_HAS_SDL3_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

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

#if MICROIDE_HAS_SDL3_TTF

bool IsRedDominant(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return a > 0 && r >= 24 && r > g + 12 && r > b + 12;
}

bool IsGreenDominant(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return a > 0 && g >= 24 && g > r + 12 && g > b + 12;
}

void EnsureReferenceTtfInitialized() {
  if (TTF_WasInit() > 0) {
    return;
  }
  Expect(TTF_Init(), "SDL_ttf should initialize for renderer reference checks");
}

struct PixelBounds {
  int left = 0;
  int right = -1;
  int top = 0;
  int bottom = -1;

  bool valid() const { return right >= left && bottom >= top; }
  int width() const { return valid() ? right - left + 1 : 0; }
};

PixelBounds NonBackgroundBounds(SDL_Surface* surface, SDL_Color background) {
  Expect(surface != nullptr, "pixel bounds should receive a readable surface");
  PixelBounds bounds;
  bool found = false;
  for (int y = 0; y < surface->h; ++y) {
    for (int x = 0; x < surface->w; ++x) {
      Uint8 r = 0;
      Uint8 g = 0;
      Uint8 b = 0;
      Uint8 a = 0;
      Expect(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a),
             "pixel bounds should read software pixels");
      if (r == background.r && g == background.g && b == background.b && a == background.a) {
        continue;
      }
      if (!found) {
        bounds.left = bounds.right = x;
        bounds.top = bounds.bottom = y;
        found = true;
      } else {
        bounds.left = std::min(bounds.left, x);
        bounds.right = std::max(bounds.right, x);
        bounds.top = std::min(bounds.top, y);
        bounds.bottom = std::max(bounds.bottom, y);
      }
    }
  }
  return bounds;
}

std::size_t CountPixelDifferences(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr,
         "pixel comparisons should receive readable surfaces");
  Expect(lhs->w == rhs->w && lhs->h == rhs->h,
         "pixel comparisons should use canvases with matching dimensions");

  std::size_t differences = 0;
  for (int y = 0; y < lhs->h; ++y) {
    for (int x = 0; x < lhs->w; ++x) {
      Uint8 lhs_r = 0;
      Uint8 lhs_g = 0;
      Uint8 lhs_b = 0;
      Uint8 lhs_a = 0;
      Uint8 rhs_r = 0;
      Uint8 rhs_g = 0;
      Uint8 rhs_b = 0;
      Uint8 rhs_a = 0;
      Expect(SDL_ReadSurfacePixel(lhs, x, y, &lhs_r, &lhs_g, &lhs_b, &lhs_a),
             "pixel comparisons should read actual pixels");
      Expect(SDL_ReadSurfacePixel(rhs, x, y, &rhs_r, &rhs_g, &rhs_b, &rhs_a),
             "pixel comparisons should read reference pixels");
      if (lhs_r != rhs_r || lhs_g != rhs_g || lhs_b != rhs_b || lhs_a != rhs_a) {
        ++differences;
      }
    }
  }
  return differences;
}

int MaxInteriorGap(SDL_Surface* surface, SDL_Color background, const PixelBounds& bounds) {
  if (surface == nullptr || !bounds.valid()) {
    return 0;
  }

  int current_gap = 0;
  int max_gap = 0;
  bool seen_occupied = false;
  for (int x = bounds.left; x <= bounds.right; ++x) {
    bool occupied = false;
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
      Uint8 r = 0;
      Uint8 g = 0;
      Uint8 b = 0;
      Uint8 a = 0;
      Expect(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a),
             "gap checks should read software pixels");
      if (r != background.r || g != background.g || b != background.b || a != background.a) {
        occupied = true;
        break;
      }
    }

    if (occupied) {
      seen_occupied = true;
      max_gap = std::max(max_gap, current_gap);
      current_gap = 0;
      continue;
    }
    if (seen_occupied) {
      ++current_gap;
    }
  }
  return max_gap;
}

void TestSdlTtfAsciiUiLabelsDoNotIntroduceExtraGlyphGaps() {
  EnsureDummySdlVideo();
  EnsureReferenceTtfInitialized();

  const std::filesystem::path font_path = TestRoot().parent_path() / "assets" / "fonts" /
                                          "JetBrainsMono-Regular.ttf";
  Expect(std::filesystem::exists(font_path),
         "renderer reference check should find the bundled monospace font");
  const SDL_Color background = SDL_Color{0x00, 0x00, 0x00, 0xff};
  const SDL_Color foreground = SDL_Color{0xff, 0xff, 0xff, 0xff};

  const auto expect_label_matches = [&](std::string_view label, bool draw_on_background) {
    SoftwareCanvas actual_canvas(640, 96);
    SoftwareCanvas reference_canvas(640, 96);

    microide::render::TextRenderer renderer;
    renderer.EnsureInitialized(actual_canvas.renderer());
    Expect(renderer.BackendName() == "sdl3_ttf",
           "UI label spacing regression test should exercise the real font backend");

    Expect(SDL_SetRenderDrawColor(actual_canvas.renderer(), background.r, background.g,
                                  background.b, background.a),
           "UI label spacing test should set the actual canvas background");
    Expect(SDL_RenderClear(actual_canvas.renderer()),
           "UI label spacing test should clear the actual canvas");
    Expect(SDL_SetRenderDrawColor(reference_canvas.renderer(), background.r, background.g,
                                  background.b, background.a),
           "UI label spacing test should set the reference canvas background");
    Expect(SDL_RenderClear(reference_canvas.renderer()),
           "UI label spacing test should clear the reference canvas");

    if (draw_on_background) {
      renderer.DrawStringOn(actual_canvas.renderer(), 12.0f, 18.0f, foreground, background, label);
    } else {
      renderer.DrawString(actual_canvas.renderer(), 12.0f, 18.0f, foreground, label);
    }

    TTF_Font* reference_font = TTF_OpenFont(font_path.string().c_str(), 13.0f);
    Expect(reference_font != nullptr, "UI label spacing test should open the bundled font");
    TTF_SetFontHinting(reference_font, TTF_HINTING_LIGHT_SUBPIXEL);
    SDL_Surface* reference_surface = draw_on_background
                                         ? TTF_RenderText_LCD(reference_font, label.data(),
                                                              label.size(), foreground, background)
                                         : TTF_RenderText_Blended(reference_font, label.data(),
                                                                  label.size(), foreground);
    Expect(reference_surface != nullptr,
           "UI label spacing test should render the reference SDL_ttf string");
    SDL_Texture* reference_texture =
        SDL_CreateTextureFromSurface(reference_canvas.renderer(), reference_surface);
    Expect(reference_texture != nullptr,
           "UI label spacing test should upload the reference SDL_ttf string");
    const SDL_FRect destination =
        SDL_FRect{12.0f, 18.0f, static_cast<float>(reference_surface->w),
                  static_cast<float>(reference_surface->h)};
    SDL_RenderTexture(reference_canvas.renderer(), reference_texture, nullptr, &destination);

    SDL_DestroyTexture(reference_texture);
    SDL_DestroySurface(reference_surface);
    TTF_CloseFont(reference_font);

    SDL_Surface* actual_pixels = SDL_RenderReadPixels(actual_canvas.renderer(), nullptr);
    SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
    const PixelBounds actual_bounds = NonBackgroundBounds(actual_pixels, background);
    const PixelBounds reference_bounds = NonBackgroundBounds(reference_pixels, background);
    const std::size_t pixel_differences =
        CountPixelDifferences(actual_pixels, reference_pixels);

    Expect(actual_bounds.valid() && reference_bounds.valid(),
           "UI label spacing test should see drawn pixels on both canvases");
    Expect(pixel_differences == 0,
           "ASCII string rendering should match SDL_ttf exactly instead of approximating glyph spacing");
    Expect(MaxInteriorGap(actual_pixels, background, actual_bounds) <=
               MaxInteriorGap(reference_pixels, background, reference_bounds) + 1,
           "ASCII string rendering should not introduce larger internal glyph gaps than SDL_ttf string rendering");
    Expect(actual_bounds.width() == reference_bounds.width(),
           "ASCII string rendering should keep the same overall width as SDL_ttf string rendering");

    SDL_DestroySurface(actual_pixels);
    SDL_DestroySurface(reference_pixels);
  };

  for (std::string_view label :
       {"bash", "dolfin-app", "function", "resolveInputPath", "path",
        "return inputPath", "if (path.isAbsolute(inputPath))"}) {
    expect_label_matches(label, false);
    expect_label_matches(label, true);
  }
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

void TestDecoratedTextGridRendererPaintsRowFillAndUnderline() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(160, 48);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "decorated text grid test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "decorated text grid test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  microide::editor::DecoratedTextGridRenderer renderer;
  microide::editor::DecoratedTextRow row;
  row.fills.push_back(microide::editor::DecoratedTextFill{
      .rect = SDL_FRect{8.0f, 6.0f, 112.0f, 14.0f},
      .color = SDL_Color{0x10, 0x40, 0x70, 0xff},
  });
  row.underlines.push_back(microide::editor::DecoratedUnderline{
      .rect = SDL_FRect{32.0f, 18.0f, 32.0f, 1.0f},
      .color = SDL_Color{0xd0, 0x30, 0x20, 0xff},
  });

  renderer.RenderRow(canvas.renderer(), text_renderer, row);

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "decorated text grid test should read software pixels");

  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  Expect(SDL_ReadSurfacePixel(pixels, 12, 10, &r, &g, &b, &a),
         "decorated text grid test should read the row fill pixel");
  Expect(r == 0x10 && g == 0x40 && b == 0x70 && a == 0xff,
         "decorated text grid renderer should paint the full row fill before text");

  Expect(SDL_ReadSurfacePixel(pixels, 40, 17, &r, &g, &b, &a),
         "decorated text grid test should read the pixel above the underline");
  Expect(r == 0x10 && g == 0x40 && b == 0x70 && a == 0xff,
         "decorated text grid renderer should preserve the row fill above the underline band");

  Expect(SDL_ReadSurfacePixel(pixels, 40, 18, &r, &g, &b, &a),
         "decorated text grid test should read the underline pixel");
  Expect(r == 0xd0 && g == 0x30 && b == 0x20 && a == 0xff,
         "decorated text grid renderer should paint the underline without recoloring the whole row");

  Expect(SDL_ReadSurfacePixel(pixels, 132, 10, &r, &g, &b, &a),
         "decorated text grid test should read a pixel outside the fill");
  Expect(r == 0x00 && g == 0x00 && b == 0x00 && a == 0xff,
         "decorated text grid renderer should leave pixels outside the row fill untouched");

  SDL_DestroySurface(pixels);
}

void TestEditorViewRendererPaintsSelectedRowsAndInlineHighlightsThroughDecoratedGrid() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "editor renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "editor renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = SDL_Color{0x08, 0x08, 0x08, 0xff};
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};
  theme.row_highlight = SDL_Color{0x24, 0x44, 0x64, 0xff};
  theme.selection_fill = SDL_Color{0x74, 0x24, 0x24, 0xff};
  theme.search_match = SDL_Color{0x24, 0x74, 0x24, 0xff};
  theme.search_match_active = SDL_Color{0x24, 0x24, 0x74, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("alpha beta gamma\nomega\n", "/tmp/editor-render.cpp");
  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorTo(0, 10, true);

  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport,
                  SDL_FRect{0.0f, 0.0f, 220.0f, 72.0f}, false, "alpha",
                  microide::editor::SelectionRange{
                      .start = microide::editor::TextPosition{0, 0},
                      .end = microide::editor::TextPosition{0, 5},
                  });

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "editor renderer test should read software pixels");

  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  Expect(SDL_ReadSurfacePixel(pixels, 5, 10, &r, &g, &b, &a),
         "editor renderer test should read the selected gutter pixel");
  Expect(r == theme.row_highlight.r && g == theme.row_highlight.g &&
             b == theme.row_highlight.b && a == theme.row_highlight.a,
         "selected editor rows should paint the whole row highlight across the gutter");

  Expect(SDL_ReadSurfacePixel(pixels, 61, 10, &r, &g, &b, &a),
         "editor renderer test should read the active search pixel");
  Expect(r == theme.search_match_active.r && g == theme.search_match_active.g &&
             b == theme.search_match_active.b && a == theme.search_match_active.a,
         "active search matches should sit above the row fill inside the shared decorated grid");

  Expect(SDL_ReadSurfacePixel(pixels, 67, 10, &r, &g, &b, &a),
         "editor renderer test should read the selection pixel");
  Expect(r == theme.selection_fill.r && g == theme.selection_fill.g &&
             b == theme.selection_fill.b && a == theme.selection_fill.a,
         "editor selections should paint as inline fills on top of the selected-row background");

  Expect(SDL_ReadSurfacePixel(pixels, 71, 10, &r, &g, &b, &a),
         "editor renderer test should read the plain selected-row pixel");
  Expect(r == theme.row_highlight.r && g == theme.row_highlight.g &&
             b == theme.row_highlight.b && a == theme.row_highlight.a,
         "editor text outside inline highlights should preserve the whole-row selected background");

  Expect(SDL_ReadSurfacePixel(pixels, 5, 24, &r, &g, &b, &a),
         "editor renderer test should read the unselected gutter pixel");
  Expect(r == theme.gutter_background.r && g == theme.gutter_background.g &&
             b == theme.gutter_background.b && a == theme.gutter_background.a,
         "unselected editor rows should keep the gutter background when no row fill is present");

  SDL_DestroySurface(pixels);
}

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

void TestMeasureWidthCacheStatsTrackQueriesAndHits() {
  microide::render::TextRenderer renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  TextRendererTestAccess::SetBackend(renderer, std::move(backend));

  renderer.ResetCacheStats();
  (void)renderer.MeasureWidth("status");
  (void)renderer.MeasureWidth("status");
  (void)renderer.MeasureWidth("branch");

  const auto stats = renderer.CacheStats();
  Expect(stats.width_cache_queries == 3,
         "width cache stats should count every measure-width query");
  Expect(stats.width_cache_hits == 1,
         "width cache stats should count repeated lookups as cache hits");
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
  AddTest(tests,
          "TextRenderer decorated grid paints row fills and underlines separately",
          TestDecoratedTextGridRendererPaintsRowFillAndUnderline);
  AddTest(tests,
          "TextRenderer editor view composes selected rows and inline highlights through decorated rows",
          TestEditorViewRendererPaintsSelectedRowsAndInlineHighlightsThroughDecoratedGrid);
  AddTest(tests, "TextRenderer caches repeated width lookups", TestMeasureWidthCachesRepeatedStrings);
  AddTest(tests,
          "TextRenderer invalidates width cache on scale changes",
          TestMeasureWidthCacheClearsWhenPresentationScaleChanges);
  AddTest(tests,
          "TextRenderer cache stats track width queries and hits",
          TestMeasureWidthCacheStatsTrackQueriesAndHits);
  AddTest(tests,
          "TextRenderer truncation uses bounded width probes",
          TestTruncateToWidthUsesUtf8BoundariesAndFewMeasurements);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests,
          "TextRenderer SDL_ttf ASCII UI labels avoid extra glyph gaps",
          TestSdlTtfAsciiUiLabelsDoNotIntroduceExtraGlyphGaps);
  AddTest(tests,
          "TextRenderer SDL_ttf ASCII glyphs stay within line bands",
          TestSdlTtfAsciiGlyphsStayWithinLineBands);
#endif
}

}  // namespace microide::tests
