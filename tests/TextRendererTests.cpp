#include "TestSupport.h"

#include "editor/DiagnosticsRender.h"
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

std::size_t CountPixelDifferencesInRect(SDL_Surface* lhs,
                                        SDL_Surface* rhs,
                                        int x,
                                        int y,
                                        int width,
                                        int height) {
  Expect(lhs != nullptr && rhs != nullptr,
         "rect pixel comparisons should receive readable surfaces");
  std::size_t differences = 0;
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      Uint8 lhs_r = 0;
      Uint8 lhs_g = 0;
      Uint8 lhs_b = 0;
      Uint8 lhs_a = 0;
      Uint8 rhs_r = 0;
      Uint8 rhs_g = 0;
      Uint8 rhs_b = 0;
      Uint8 rhs_a = 0;
      Expect(SDL_ReadSurfacePixel(lhs, x + col, y + row, &lhs_r, &lhs_g, &lhs_b, &lhs_a),
             "rect pixel comparisons should read actual pixels");
      Expect(SDL_ReadSurfacePixel(rhs, x + col, y + row, &rhs_r, &rhs_g, &rhs_b, &rhs_a),
             "rect pixel comparisons should read reference pixels");
      if (lhs_r != rhs_r || lhs_g != rhs_g || lhs_b != rhs_b || lhs_a != rhs_a) {
        ++differences;
      }
    }
  }
  return differences;
}

void TestSdlTtfAsciiUiLabelsDoNotIntroduceExtraGlyphGaps() {
  EnsureDummySdlVideo();
  const SDL_Color background = SDL_Color{0x00, 0x00, 0x00, 0xff};
  const SDL_Color foreground = SDL_Color{0xff, 0xff, 0xff, 0xff};

  const auto expect_label_matches = [&](std::string_view left, std::string_view right) {
    const std::string unsplit_label = std::string(left) + std::string(right);
    SoftwareCanvas unsplit_canvas(640, 96);
    SoftwareCanvas split_canvas(640, 96);

    microide::render::TextRenderer unsplit_renderer;
    microide::render::TextRenderer split_renderer;
    unsplit_renderer.EnsureInitialized(unsplit_canvas.renderer());
    split_renderer.EnsureInitialized(split_canvas.renderer());
    Expect(unsplit_renderer.BackendName() == "sdl3_ttf" &&
               split_renderer.BackendName() == "sdl3_ttf",
           "UI label spacing regression test should exercise the real font backend");

    Expect(SDL_SetRenderDrawColor(unsplit_canvas.renderer(), background.r, background.g,
                                  background.b, background.a),
           "UI label spacing test should set the unsplit canvas background");
    Expect(SDL_RenderClear(unsplit_canvas.renderer()),
           "UI label spacing test should clear the unsplit canvas");
    Expect(SDL_SetRenderDrawColor(split_canvas.renderer(), background.r, background.g,
                                  background.b, background.a),
           "UI label spacing test should set the split canvas background");
    Expect(SDL_RenderClear(split_canvas.renderer()),
           "UI label spacing test should clear the split canvas");

    unsplit_renderer.DrawString(unsplit_canvas.renderer(), 12.0f, 18.0f, foreground,
                                unsplit_label);
    split_renderer.DrawString(split_canvas.renderer(), 12.0f, 18.0f, foreground, left);
    split_renderer.DrawString(split_canvas.renderer(),
                              12.0f + split_renderer.MeasureWidth(left), 18.0f, foreground,
                              right);

    SDL_Surface* unsplit_pixels = SDL_RenderReadPixels(unsplit_canvas.renderer(), nullptr);
    SDL_Surface* split_pixels = SDL_RenderReadPixels(split_canvas.renderer(), nullptr);
    const PixelBounds unsplit_bounds = NonBackgroundBounds(unsplit_pixels, background);
    const PixelBounds split_bounds = NonBackgroundBounds(split_pixels, background);
    const std::size_t pixel_differences = CountPixelDifferences(unsplit_pixels, split_pixels);

    Expect(unsplit_bounds.valid() && split_bounds.valid(),
           "UI label spacing test should see drawn pixels on both canvases");
    Expect(pixel_differences == 0,
           "ASCII string rendering should keep identical glyph placement when a row is split into runs");
    Expect(unsplit_bounds.width() == split_bounds.width(),
           "ASCII string rendering should keep the same overall width when runs are split");

    SDL_DestroySurface(unsplit_pixels);
    SDL_DestroySurface(split_pixels);
  };

  expect_label_matches("bash", "");
  expect_label_matches("dolfin-", "app");
  expect_label_matches("resolve", "InputPath");
  expect_label_matches("return ", "inputPath");
  expect_label_matches("if (path.", "isAbsolute(inputPath))");
}

void TestSdlTtfAsciiRepeatedGlyphsMeasureByCharWidth() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(320, 120);
  microide::render::TextRenderer renderer;
  renderer.EnsureInitialized(canvas.renderer());
  Expect(renderer.BackendName() == "sdl3_ttf",
         "ASCII width regression should exercise the SDL_ttf backend");
  const float char_width = renderer.CharWidth();
  Expect(std::abs(renderer.MeasureWidth("!") - char_width) <= 0.01f,
         "single glyph width should match one character width");
  Expect(std::abs(renderer.MeasureWidth("!!") - (2.0f * char_width)) <= 0.01f,
         "double glyph width should match two character widths");
  Expect(std::abs(renderer.MeasureWidth("!!!") - (3.0f * char_width)) <= 0.01f,
         "triple glyph width should match three character widths");
}

void TestSdlTtfAsciiPrefixGlyphStaysFixedWhenAppendingMatches() {
  EnsureDummySdlVideo();
  SoftwareCanvas single_canvas(320, 96);
  SoftwareCanvas triple_canvas(320, 96);
  microide::render::TextRenderer single_renderer;
  microide::render::TextRenderer triple_renderer;
  single_renderer.EnsureInitialized(single_canvas.renderer());
  triple_renderer.EnsureInitialized(triple_canvas.renderer());
  Expect(single_renderer.BackendName() == "sdl3_ttf" &&
             triple_renderer.BackendName() == "sdl3_ttf",
         "ASCII prefix stability test should exercise the SDL_ttf backend");

  const SDL_Color background = SDL_Color{0, 0, 0, 255};
  const SDL_Color foreground = SDL_Color{255, 0, 0, 255};
  Expect(SDL_SetRenderDrawColor(single_canvas.renderer(), background.r, background.g, background.b,
                                background.a),
         "ASCII prefix stability test should set the single canvas background");
  Expect(SDL_RenderClear(single_canvas.renderer()),
         "ASCII prefix stability test should clear the single canvas");
  Expect(SDL_SetRenderDrawColor(triple_canvas.renderer(), background.r, background.g, background.b,
                                background.a),
         "ASCII prefix stability test should set the triple canvas background");
  Expect(SDL_RenderClear(triple_canvas.renderer()),
         "ASCII prefix stability test should clear the triple canvas");

  single_renderer.DrawString(single_canvas.renderer(), 12.0f, 18.0f, foreground, "!");
  triple_renderer.DrawString(triple_canvas.renderer(), 12.0f, 18.0f, foreground, "!!!");

  SDL_Surface* single_pixels = SDL_RenderReadPixels(single_canvas.renderer(), nullptr);
  SDL_Surface* triple_pixels = SDL_RenderReadPixels(triple_canvas.renderer(), nullptr);
  Expect(single_pixels != nullptr && triple_pixels != nullptr,
         "ASCII prefix stability test should read back both canvases");

  const int cell_width = static_cast<int>(std::ceil(single_renderer.CharWidth()));
  const int cell_height = static_cast<int>(std::ceil(single_renderer.LineHeight()));
  const std::size_t differences =
      CountPixelDifferencesInRect(single_pixels, triple_pixels, 12, 18, cell_width, cell_height);

  SDL_DestroySurface(single_pixels);
  SDL_DestroySurface(triple_pixels);

  Expect(differences == 0,
         "the first glyph cell should be identical whether rendered alone or with appended matches");
}

void TestSdlTtfBlendedTextKeepsTransparentCorners() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(160, 80);
  microide::render::TextRenderer renderer;
  renderer.EnsureInitialized(canvas.renderer());
  Expect(renderer.BackendName() == "sdl3_ttf",
         "blended-corner regression should exercise the SDL_ttf backend");

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 0),
         "blended-corner regression should set transparent canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "blended-corner regression should clear the canvas");
  renderer.DrawStringOn(canvas.renderer(), 20.0f, 20.0f, SDL_Color{255, 255, 255, 255},
                        SDL_Color{10, 20, 30, 255}, "abc");

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "blended-corner regression should read rendered pixels");
  Uint8 r = 0, g = 0, b = 0, a = 0;
  Expect(SDL_ReadSurfacePixel(pixels, 20, 20, &r, &g, &b, &a),
         "blended-corner regression should read top-left pixel");
  Expect(a == 0, "top-left blended text pixel should stay transparent");
  const int right_x = 20 + static_cast<int>(std::ceil(renderer.MeasureWidth("abc"))) - 1;
  const int bottom_y = 20 + static_cast<int>(std::ceil(renderer.LineHeight())) - 1;
  Expect(SDL_ReadSurfacePixel(pixels, std::max(0, right_x), std::max(0, bottom_y), &r, &g, &b, &a),
         "blended-corner regression should read bottom-right pixel");
  Expect(a == 0, "bottom-right blended text pixel should stay transparent");
  SDL_DestroySurface(pixels);
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
  Expect(r == 0xd0 && g == 0x30 && b == 0x20 &&
             a == static_cast<Uint8>(std::lround(0xff * 0.55)),
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

void TestEditorViewRendererPaintsDiagnosticUnderlines() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "diagnostic underline renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "diagnostic underline renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = SDL_Color{0x08, 0x08, 0x08, 0xff};
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};
  theme.diagnostic_warning = SDL_Color{0xe0, 0xbc, 0x6d, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("alpha beta\nomega\n", "/tmp/editor-diagnostics.cpp");
  viewport.MoveCursorTo(1, 0);

  const std::vector<microide::editor::PublishedDiagnostic> diagnostics = {
      microide::editor::PublishedDiagnostic{
          .owner = "eslint",
          .path = "/tmp/editor-diagnostics.cpp",
          .range =
              microide::editor::SelectionRange{
                  .start = microide::editor::TextPosition{0, 6},
                  .end = microide::editor::TextPosition{0, 10},
              },
          .severity = microide::editor::DiagnosticSeverity::Warning,
          .message = "unused value",
      },
  };

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 72.0f};
  const auto metrics = microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);

  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "", std::nullopt,
                  std::nullopt, diagnostics);

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "diagnostic underline renderer test should read software pixels");

  const int underline_x = static_cast<int>(metrics.text_x) + 7;
  const int underline_y =
      static_cast<int>(metrics.first_line_y + metrics.line_height - 2.0f);

  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  Expect(SDL_ReadSurfacePixel(pixels, underline_x, underline_y - 1, &r, &g, &b, &a),
         "diagnostic underline renderer test should read the pixel above the underline");
  Expect(r == theme.editor_background.r && g == theme.editor_background.g &&
             b == theme.editor_background.b && a == theme.editor_background.a,
         "diagnostic underlines should not recolor the whole editor row");

  Expect(SDL_ReadSurfacePixel(pixels, underline_x, underline_y, &r, &g, &b, &a),
         "diagnostic underline renderer test should read the underline pixel");
  Expect(r == theme.diagnostic_warning.r && g == theme.diagnostic_warning.g &&
             b == theme.diagnostic_warning.b &&
             a == static_cast<Uint8>(std::lround(theme.diagnostic_warning.a * 0.55)),
         "diagnostic underlines should use the severity color from the theme");

  SDL_DestroySurface(pixels);
}

void TestHighestDiagnosticSeverityForLinePrefersTheMostSevereMatch() {
  const std::vector<microide::editor::PublishedDiagnostic> diagnostics = {
      microide::editor::PublishedDiagnostic{
          .owner = "lint",
          .path = "/tmp/line-severity.cpp",
          .range =
              microide::editor::SelectionRange{
                  .start = microide::editor::TextPosition{0, 0},
                  .end = microide::editor::TextPosition{0, 4},
              },
          .severity = microide::editor::DiagnosticSeverity::Warning,
          .message = "warning",
      },
      microide::editor::PublishedDiagnostic{
          .owner = "lint",
          .path = "/tmp/line-severity.cpp",
          .range =
              microide::editor::SelectionRange{
                  .start = microide::editor::TextPosition{0, 2},
                  .end = microide::editor::TextPosition{1, 2},
              },
          .severity = microide::editor::DiagnosticSeverity::Error,
          .message = "error",
      },
      microide::editor::PublishedDiagnostic{
          .owner = "lint",
          .path = "/tmp/line-severity.cpp",
          .range =
              microide::editor::SelectionRange{
                  .start = microide::editor::TextPosition{2, 0},
                  .end = microide::editor::TextPosition{2, 1},
              },
          .severity = microide::editor::DiagnosticSeverity::Hint,
          .message = "hint",
      },
  };

  Expect(microide::editor::HighestDiagnosticSeverityForLine(diagnostics, 0).has_value() &&
             *microide::editor::HighestDiagnosticSeverityForLine(diagnostics, 0) ==
                 microide::editor::DiagnosticSeverity::Error,
         "line severity should prefer the most severe overlapping diagnostic");
  Expect(microide::editor::HighestDiagnosticSeverityForLine(diagnostics, 1).has_value() &&
             *microide::editor::HighestDiagnosticSeverityForLine(diagnostics, 1) ==
                 microide::editor::DiagnosticSeverity::Error,
         "line severity should treat multi-line diagnostics as covering each intersected line");
  Expect(microide::editor::HighestDiagnosticSeverityForLine(diagnostics, 3) == std::nullopt,
         "line severity should report no marker when a line has no diagnostics");
}

void TestEditorViewRendererPaintsDiagnosticGutterMarkers() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "diagnostic gutter renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "diagnostic gutter renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = SDL_Color{0x08, 0x08, 0x08, 0xff};
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};
  theme.diagnostic_error = SDL_Color{0xd9, 0x64, 0x64, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("alpha\nomega\n", "/tmp/editor-diagnostic-gutter.cpp");
  viewport.MoveCursorTo(1, 0);

  const std::vector<microide::editor::PublishedDiagnostic> diagnostics = {
      microide::editor::PublishedDiagnostic{
          .owner = "eslint",
          .path = "/tmp/editor-diagnostic-gutter.cpp",
          .range =
              microide::editor::SelectionRange{
                  .start = microide::editor::TextPosition{0, 1},
                  .end = microide::editor::TextPosition{0, 4},
              },
          .severity = microide::editor::DiagnosticSeverity::Error,
          .message = "unexpected token",
      },
  };

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 72.0f};
  const auto metrics =
      microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  const SDL_FRect marker_rect = microide::editor::DiagnosticGutterMarkerRect(
      rect.x, metrics.first_line_y, metrics.gutter_width, metrics.line_height);

  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "", std::nullopt,
                  std::nullopt, diagnostics);

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "diagnostic gutter renderer test should read software pixels");

  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int marker_x = static_cast<int>(std::floor(marker_rect.x + marker_rect.w * 0.5f));
  const int marker_y = static_cast<int>(std::floor(marker_rect.y + marker_rect.h * 0.5f));
  Expect(SDL_ReadSurfacePixel(pixels, marker_x, marker_y, &r, &g, &b, &a),
         "diagnostic gutter renderer test should read the marker pixel");
  Expect(r == theme.diagnostic_error.r && g == theme.diagnostic_error.g &&
             b == theme.diagnostic_error.b && a == theme.diagnostic_error.a,
         "diagnostic gutter markers should use the severity color");

  Expect(SDL_ReadSurfacePixel(pixels, static_cast<int>(marker_rect.x + marker_rect.w + 3.0f),
                              marker_y, &r, &g, &b, &a),
         "diagnostic gutter renderer test should read the plain gutter pixel");
  Expect(r == theme.gutter_background.r && g == theme.gutter_background.g &&
             b == theme.gutter_background.b && a == theme.gutter_background.a,
         "diagnostic gutter markers should not recolor the whole gutter");

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
  AddTest(tests,
          "TextRenderer editor view paints diagnostic underlines",
          TestEditorViewRendererPaintsDiagnosticUnderlines);
  AddTest(tests,
          "TextRenderer diagnostic line severity prefers the most severe match",
          TestHighestDiagnosticSeverityForLinePrefersTheMostSevereMatch);
  AddTest(tests,
          "TextRenderer editor view paints diagnostic gutter markers",
          TestEditorViewRendererPaintsDiagnosticGutterMarkers);
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
  AddTest(tests,
          "TextRenderer SDL_ttf repeated glyphs measure by char width",
          TestSdlTtfAsciiRepeatedGlyphsMeasureByCharWidth);
  AddTest(tests,
          "TextRenderer SDL_ttf ASCII prefix glyph stays fixed when appending matches",
          TestSdlTtfAsciiPrefixGlyphStaysFixedWhenAppendingMatches);
  AddTest(tests,
          "TextRenderer SDL_ttf blended text keeps transparent corners",
          TestSdlTtfBlendedTextKeepsTransparentCorners);
#endif
}

}  // namespace microide::tests
