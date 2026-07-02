#include "TestSupport.h"

#include "editor/DiagnosticsRender.h"
#include "editor/DecoratedTextGridRenderer.h"
#include "editor/EditorViewRenderer.h"
#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "platform/RuntimePaths.h"
#include "render/AsciiGlyphAtlas.h"
#include "render/PixelAlign.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceContext.h"

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
  struct DrawCall {
    float x = 0.0f;
    float y = 0.0f;
    SDL_Color color{};
    SDL_Color background{};
    std::string text;
    bool with_background = false;
  };

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
    draw_calls_.push_back(DrawCall{
        .x = x,
        .y = y,
        .color = color,
        .background = SDL_Color{},
        .text = std::string(text),
        .with_background = false,
    });
  }

  void DrawStringOn(SDL_Renderer* renderer,
                    float x,
                    float y,
                    SDL_Color color,
                    SDL_Color background,
                    std::string_view text) override {
    (void) renderer;
    draw_calls_.push_back(DrawCall{
        .x = x,
        .y = y,
        .color = color,
        .background = background,
        .text = std::string(text),
        .with_background = true,
    });
  }

  int measure_width_calls() const { return measure_width_calls_; }
  const std::vector<DrawCall>& draw_calls() const { return draw_calls_; }
  void ResetDrawCalls() { draw_calls_.clear(); }

 private:
  mutable int measure_width_calls_ = 0;
  float scale_x_ = 1.0f;
  float scale_y_ = 1.0f;
  std::vector<DrawCall> draw_calls_;
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

const CountingTextBackend::DrawCall* FindDrawCall(const std::vector<CountingTextBackend::DrawCall>& calls,
                                                  std::string_view text,
                                                  bool with_background) {
  const auto it = std::find_if(calls.begin(), calls.end(),
                               [&](const CountingTextBackend::DrawCall& call) {
                                 return call.text == text && call.with_background == with_background;
                               });
  return it == calls.end() ? nullptr : &*it;
}

std::size_t CountDrawCalls(const std::vector<CountingTextBackend::DrawCall>& calls,
                           std::string_view text,
                           bool with_background) {
  return static_cast<std::size_t>(std::count_if(
      calls.begin(), calls.end(), [&](const CountingTextBackend::DrawCall& call) {
        return call.text == text && call.with_background == with_background;
      }));
}

std::string SummarizeDrawCalls(const std::vector<CountingTextBackend::DrawCall>& calls) {
  std::string summary;
  for (const auto& call : calls) {
    if (!summary.empty()) {
      summary += " | ";
    }
    summary += call.with_background ? "bg:" : "fg:";
    summary += call.text;
  }
  return summary;
}

microide::editor::FoldingModel::ComputeOptions DefaultFoldOptions() {
  microide::editor::FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

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
  expect_label_matches("example-", "app");
  expect_label_matches("resolve", "InputPath");
  expect_label_matches("return ", "inputPath");
  expect_label_matches("if (path.", "isAbsolute(inputPath))");
}

// Opens a monospace TTF font for atlas tests, mirroring the backend's discovery
// order (bundled JetBrains Mono first, then common system monospace fonts) and
// the same hinting/kerning configuration. Returns nullptr if none is available.
TTF_Font* OpenTestMonospaceFont() {
  Expect(TTF_Init(), "atlas tests should initialize SDL_ttf");
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(platform::ResolveBundledAssetPath("fonts/JetBrainsMono-Regular.ttf"));
  for (const char* system_candidate : {
           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
           "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
           "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
       }) {
    candidates.emplace_back(system_candidate);
  }
  for (const auto& candidate : candidates) {
    if (candidate.empty() || !std::filesystem::exists(candidate)) {
      continue;
    }
    TTF_Font* font = TTF_OpenFont(candidate.string().c_str(), 13.0f);
    if (font != nullptr) {
      TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
      TTF_SetFontKerning(font, false);
      return font;
    }
  }
  return nullptr;
}

// The atlas stores one colour-independent (white) coverage bitmap per glyph and
// tints it via colour modulation when assembling a composite. This proves the
// tinted atlas blit is byte-identical to rendering the glyph directly at that
// colour, which is the invariant that lets the per-(char, colour) glyph cache be
// replaced by a single atlas without changing on-screen pixels.
void TestAsciiGlyphAtlasMatchesPerColorRendering() {
  EnsureDummySdlVideo();
  TTF_Font* font = OpenTestMonospaceFont();
  Expect(font != nullptr, "atlas pixel-identity test should find a usable monospace font");

  auto atlas = microide::render::AsciiGlyphAtlas::Build(font);
  Expect(atlas != nullptr, "atlas should build from a usable font");

  const std::vector<SDL_Color> colors = {
      SDL_Color{0xff, 0xff, 0xff, 0xff},  // white (the atlas's stored colour)
      SDL_Color{0xff, 0x00, 0x00, 0xff},  // red
      SDL_Color{0x00, 0xff, 0x00, 0xff},  // green
      SDL_Color{0x56, 0x9c, 0xd6, 0xff},  // a typical keyword blue
      SDL_Color{0x80, 0x40, 0x20, 0xff},  // an asymmetric tone that exercises rounding
  };
  const std::string sample = "Ag0 {}[]();<>/*-_=+!~\"'`";

  for (const SDL_Color color : colors) {
    for (const char ch : sample) {
      SDL_Surface* reference = TTF_RenderText_Blended(font, &ch, 1, color);
      Expect(reference != nullptr, "reference glyph render should succeed");

      SDL_Surface* actual =
          SDL_CreateSurface(reference->w, reference->h, SDL_PIXELFORMAT_RGBA32);
      Expect(actual != nullptr, "atlas blit target surface should allocate");
      Expect(SDL_FillSurfaceRect(actual, nullptr, 0), "atlas target should clear to transparent");

      Expect(atlas->BlitInto(actual, 0, ch, color),
             "atlas should cover every printable ASCII glyph in the sample");
      const std::size_t differences = CountPixelDifferences(actual, reference);
      Expect(differences == 0,
             "tinted atlas glyph must be pixel-identical to a direct colour render");

      SDL_DestroySurface(actual);
      SDL_DestroySurface(reference);
    }
  }

  // Translucent colours are intentionally not served by the atlas; the backend
  // falls back to the whole-string SDL_ttf path for them.
  SDL_Surface* translucent_target = SDL_CreateSurface(32, atlas->FontHeightPx(), SDL_PIXELFORMAT_RGBA32);
  Expect(translucent_target != nullptr, "translucent target should allocate");
  Expect(!atlas->BlitInto(translucent_target, 0, 'A', SDL_Color{0xff, 0xff, 0xff, 0x80}),
         "atlas must decline translucent colours so the backend uses its exact fallback");
  SDL_DestroySurface(translucent_target);

  TTF_CloseFont(font);
}

void TestAsciiGlyphAtlasCoversPrintableRange() {
  EnsureDummySdlVideo();
  TTF_Font* font = OpenTestMonospaceFont();
  Expect(font != nullptr, "atlas coverage test should find a usable monospace font");

  auto atlas = microide::render::AsciiGlyphAtlas::Build(font);
  Expect(atlas != nullptr, "atlas should build from a usable font");

  SDL_Surface* scratch = SDL_CreateSurface(64, atlas->FontHeightPx(), SDL_PIXELFORMAT_RGBA32);
  Expect(scratch != nullptr, "coverage test scratch surface should allocate");
  const SDL_Color color{0xc0, 0xc0, 0xc0, 0xff};
  for (int code = 0x20; code <= 0x7E; ++code) {
    const char ch = static_cast<char>(code);
    Expect(atlas->Covers(ch), "atlas should cover the full printable ASCII range");
    // Force lazy rasterization of every slot and confirm it succeeds.
    Expect(SDL_FillSurfaceRect(scratch, nullptr, 0), "coverage scratch should clear");
    Expect(atlas->BlitInto(scratch, 0, ch, color),
           "atlas should lazily rasterize and blit every printable glyph");
  }
  SDL_DestroySurface(scratch);

  Expect(!atlas->Covers('\t'), "atlas should not claim to cover control characters");
  Expect(!atlas->Covers(static_cast<char>(0x7F)),
         "atlas should not claim to cover the delete control code");

  TTF_CloseFont(font);
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

void TestSdlTtfSetFontPointSizeResizesGlyphMetrics() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(320, 160);
  microide::render::TextRenderer renderer;
  renderer.EnsureInitialized(canvas.renderer());
  Expect(renderer.BackendName() == "sdl3_ttf",
         "font-size regression should exercise the SDL_ttf backend");

  const float base_char_width = renderer.CharWidth();
  const float base_line_height = renderer.LineHeight();
  Expect(base_char_width > 0.0f && base_line_height > 0.0f,
         "baseline font metrics should be positive");

  renderer.SetFontPointSize(24.0f);
  const float large_char_width = renderer.CharWidth();
  const float large_line_height = renderer.LineHeight();
  Expect(large_char_width > base_char_width,
         "a larger point size should widen the monospace glyph advance");
  Expect(large_line_height > base_line_height,
         "a larger point size should increase the line height");
  Expect(std::abs(renderer.MeasureWidth("MM") - 2.0f * large_char_width) <= 0.5f,
         "measured ASCII width should track the resized monospace advance");

  // Restoring the default size restores the original metrics (the backend re-opens
  // the same font at the original point size and refreshes metrics).
  renderer.SetFontPointSize(13.0f);
  Expect(std::abs(renderer.CharWidth() - base_char_width) <= 0.5f,
         "restoring the default point size should restore the baseline char width");
  Expect(std::abs(renderer.LineHeight() - base_line_height) <= 0.5f,
         "restoring the default point size should restore the baseline line height");

  // Out-of-range requests clamp into the supported 8..32 range instead of breaking
  // metrics.
  renderer.SetFontPointSize(1000.0f);
  Expect(renderer.CharWidth() > base_char_width,
         "an oversized request should clamp to the max supported size, not zero metrics");
}

void TestSdlTtfSetFontFamilyResilientToUnresolved() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(320, 160);
  microide::render::TextRenderer renderer;
  renderer.EnsureInitialized(canvas.renderer());
  Expect(renderer.BackendName() == "sdl3_ttf",
         "font-family test should exercise the SDL_ttf backend");

  const float base_char_width = renderer.CharWidth();
  Expect(base_char_width > 0.0f, "baseline metrics should be positive");

  // An unresolved family name must be a no-op: it reports no change and leaves the
  // current font (and metrics) intact rather than nulling out the backend.
  Expect(!renderer.SetFontFamily("this-font-does-not-exist-zzz"),
         "an unresolved font family should report no change");
  Expect(std::abs(renderer.CharWidth() - base_char_width) <= 0.01f,
         "an unresolved font family must keep the current font metrics");
  Expect(renderer.MeasureWidth("MM") > 0.0f,
         "text rendering stays functional after an unresolved family request");

  // Empty family resolves to the default file already open -> no visual change.
  Expect(!renderer.SetFontFamily(""),
         "empty family resolves to the current default and reports no change");
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

  // Text-region pixels sit 8px further right than before: the gutter reserves a
  // marker column so breakpoint/execution markers no longer overlap line numbers,
  // widening it from 48 to 56 here (see editor/GutterMetrics.h).
  Expect(SDL_ReadSurfacePixel(pixels, 69, 10, &r, &g, &b, &a),
         "editor renderer test should read the active search pixel");
  Expect(r == theme.search_match_active.r && g == theme.search_match_active.g &&
             b == theme.search_match_active.b && a == theme.search_match_active.a,
         "active search matches should sit above the row fill inside the shared decorated grid");

  Expect(SDL_ReadSurfacePixel(pixels, 75, 10, &r, &g, &b, &a),
         "editor renderer test should read the selection pixel");
  Expect(r == theme.selection_fill.r && g == theme.selection_fill.g &&
             b == theme.selection_fill.b && a == theme.selection_fill.a,
         "editor selections should paint as inline fills on top of the selected-row background");

  Expect(SDL_ReadSurfacePixel(pixels, 79, 10, &r, &g, &b, &a),
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

void TestEditorViewRendererUsesWrappedRowsAndSuppressesContinuationGutterNumbers() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(84, 90);

  microide::render::TextRenderer text_renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(text_renderer, std::move(backend));

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  microide::editor::TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nxy", "/tmp/editor-wrap-render.cpp");
  viewport.SetSoftWrap(true);

  const SDL_FRect rect{0.0f, 0.0f, 84.0f, 90.0f};
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false);

  const auto& calls = backend_ptr->draw_calls();
  const std::string draw_summary = SummarizeDrawCalls(calls);
  Expect(FindDrawCall(calls, "abcdefgh", false) != nullptr, draw_summary);
  Expect(FindDrawCall(calls, "ijklmnop", false) != nullptr, draw_summary);
  Expect(FindDrawCall(calls, "qrst", false) != nullptr, draw_summary);
  Expect(FindDrawCall(calls, "xy", false) != nullptr, draw_summary);

  // The counting backend does not batch runs, so gutter numbers take the inline
  // DrawStringOn path (with background), exactly as before the atlas change.
  Expect(CountDrawCalls(calls, "1", true) == 1,
         "the gutter should draw the logical line number only on the first wrapped row");
  Expect(CountDrawCalls(calls, "2", true) == 1,
         "the gutter should still draw later logical line numbers once each");
}

void TestEditorViewRendererSearchHighlightsTrackHorizontalScroll() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "horizontal-scroll search renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "horizontal-scroll search renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = SDL_Color{0x08, 0x08, 0x08, 0xff};
  theme.row_highlight = theme.editor_background;
  theme.search_match = SDL_Color{0x24, 0x74, 0x24, 0xff};
  theme.search_match_active = SDL_Color{0x24, 0x24, 0x74, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("0123456789 target suffix\n", "/tmp/editor-scroll-search.cpp");
  viewport.SetHorizontalScroll(7);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 72.0f};
  const auto metrics =
      microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "target");

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr,
         "horizontal-scroll search renderer test should read software pixels");

  const int row_y = static_cast<int>(metrics.first_line_y + 2.0f);
  const std::size_t match_start_column = viewport.lines().front().find("target");
  const std::size_t match_start_visual = microide::editor::TextLayout::VisualColumnForTextColumn(
      viewport.lines().front(), match_start_column, viewport.tab_size());
  const int expected_start_x = static_cast<int>(
      metrics.text_x + static_cast<float>(match_start_visual - viewport.horizontal_scroll()));
  const int expected_end_x = expected_start_x + static_cast<int>(std::string_view("target").size()) - 1;
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;

  Expect(SDL_ReadSurfacePixel(pixels, expected_start_x, row_y, &r, &g, &b, &a),
         "horizontal-scroll search renderer test should read the scrolled match pixel");
  Expect(r == theme.search_match.r && g == theme.search_match.g &&
             b == theme.search_match.b && a == theme.search_match.a,
         "search highlight should move left with the horizontally scrolled text");

  int first_highlight_x = -1;
  int last_highlight_x = -1;
  for (int x = 0; x < pixels->w; ++x) {
    Expect(SDL_ReadSurfacePixel(pixels, x, row_y, &r, &g, &b, &a),
           "horizontal-scroll search renderer test should scan rendered row pixels");
    if (r == theme.search_match.r && g == theme.search_match.g && b == theme.search_match.b &&
        a == theme.search_match.a) {
      if (first_highlight_x < 0) {
        first_highlight_x = x;
      }
      last_highlight_x = x;
    }
  }
  Expect(first_highlight_x == expected_start_x,
         "search highlight should start at the horizontally scrolled text position");
  Expect(last_highlight_x == expected_end_x,
         "search highlight width should stay anchored to the scrolled match span");

  SDL_DestroySurface(pixels);
}

#ifndef NDEBUG
void TestEditorViewRendererReusesWrapCacheAcrossFrames() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(84, 90);

  microide::render::TextRenderer text_renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(text_renderer, std::move(backend));

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  microide::editor::TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nxy", "/tmp/editor-wrap-cache.cpp");
  viewport.SetSoftWrap(true);

  const SDL_FRect rect{0.0f, 0.0f, 84.0f, 90.0f};
  microide::editor::EditorViewRenderer renderer;
  const std::size_t before_frames = viewport.WrappedRowLayoutBuildCountForDebug();
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false);
  const std::size_t after_first_frame = viewport.WrappedRowLayoutBuildCountForDebug();
  backend_ptr->ResetDrawCalls();

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false);
  const std::size_t after_second_frame = viewport.WrappedRowLayoutBuildCountForDebug();

  Expect(after_second_frame - before_frames <= 1,
         "two consecutive wrapped renders without edits or resize should rebuild the wrap cache at most once");
  Expect(after_second_frame == after_first_frame,
         "a second wrapped render without edits or resize should reuse the cached wrap layout");
}
#endif

void TestEditorViewRendererAdvancesPastWrappedEmptyLines() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(84, 90);

  microide::render::TextRenderer text_renderer;
  auto backend = std::make_unique<CountingTextBackend>();
  auto* backend_ptr = backend.get();
  TextRendererTestAccess::SetBackend(text_renderer, std::move(backend));

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  microide::editor::TextViewport viewport;
  viewport.LoadContent("\nabcdefghijk", "/tmp/editor-wrap-empty-line.cpp");
  viewport.SetSoftWrap(true);

  const SDL_FRect rect{0.0f, 0.0f, 84.0f, 90.0f};
  const auto metrics = microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false);

  const auto& calls = backend_ptr->draw_calls();
  const auto* line_two_gutter = FindDrawCall(calls, "2", true);
  const auto* wrapped_text = FindDrawCall(calls, "abcdefgh", false);
  const auto* wrapped_tail = FindDrawCall(calls, "ijk", false);
  Expect(line_two_gutter != nullptr,
         "rendering an empty wrapped line should still advance the gutter to the next logical line");
  Expect(wrapped_text != nullptr && std::fabs(wrapped_text->y - (metrics.first_line_y + metrics.line_height)) < 0.01f,
         "text after an empty wrapped line should render on the next visual row");
  Expect(wrapped_tail != nullptr && std::fabs(wrapped_tail->y - (metrics.first_line_y + 2.0f * metrics.line_height)) < 0.01f,
         "wrapped text after an empty line should continue onto subsequent wrapped rows");
}

void TestEditorViewRendererClipsWrappedSelectionsPerRow() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(84, 90);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "wrapped selection renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "wrapped selection renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = SDL_Color{0x08, 0x08, 0x08, 0xff};
  theme.row_highlight = theme.editor_background;
  theme.selection_fill = SDL_Color{0x74, 0x24, 0x24, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst", "/tmp/editor-wrap-selection.cpp");
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorTo(0, 18, true);

  const SDL_FRect rect{0.0f, 0.0f, 84.0f, 90.0f};
  const auto metrics = microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false);

  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "wrapped selection renderer test should read software pixels");

  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int row0_y = static_cast<int>(metrics.first_line_y + 2.0f);
  const int row1_y = static_cast<int>(metrics.first_line_y + metrics.line_height + 2.0f);
  const int row2_y = static_cast<int>(metrics.first_line_y + 2.0f * metrics.line_height + 2.0f);
  const int text_x = static_cast<int>(metrics.text_x);

  Expect(SDL_ReadSurfacePixel(pixels, text_x + 5, row0_y, &r, &g, &b, &a),
         "wrapped selection renderer test should read a pixel before the row-0 selection");
  Expect(r == theme.editor_background.r && g == theme.editor_background.g &&
             b == theme.editor_background.b && a == theme.editor_background.a,
         "wrapped selections should not backfill pixels before the row's clipped start");

  Expect(SDL_ReadSurfacePixel(pixels, text_x + 7, row0_y, &r, &g, &b, &a),
         "wrapped selection renderer test should read a row-0 selected pixel");
  Expect(r == theme.selection_fill.r && g == theme.selection_fill.g &&
             b == theme.selection_fill.b && a == theme.selection_fill.a,
         "wrapped selections should paint the visible tail of the first wrapped row");

  Expect(SDL_ReadSurfacePixel(pixels, text_x + 7, row1_y, &r, &g, &b, &a),
         "wrapped selection renderer test should read a continuation-row selected pixel");
  Expect(r == theme.selection_fill.r && g == theme.selection_fill.g &&
             b == theme.selection_fill.b && a == theme.selection_fill.a,
         "wrapped selections should fill the fully covered continuation row");

  Expect(SDL_ReadSurfacePixel(pixels, text_x + 1, row2_y, &r, &g, &b, &a),
         "wrapped selection renderer test should read a tail-row selected pixel");
  Expect(r == theme.selection_fill.r && g == theme.selection_fill.g &&
             b == theme.selection_fill.b && a == theme.selection_fill.a,
         "wrapped selections should paint the visible head of the final wrapped row");

  Expect(SDL_ReadSurfacePixel(pixels, text_x + 3, row2_y, &r, &g, &b, &a),
         "wrapped selection renderer test should read a pixel after the tail-row selection");
  Expect(r == theme.editor_background.r && g == theme.editor_background.g &&
             b == theme.editor_background.b && a == theme.editor_background.a,
         "wrapped selections should clip at the wrapped row end instead of spilling into the row tail");

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

void TestEditorViewRendererPaintsFoldGutterMarkers() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  Expect(SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255),
         "fold gutter renderer test should set the software canvas background");
  Expect(SDL_RenderClear(canvas.renderer()),
         "fold gutter renderer test should clear the software canvas");

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};
  theme.line_number = SDL_Color{0xc8, 0xd2, 0xe6, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("void f() {\n  body();\n}\n", "/tmp/editor-fold-gutter.cpp");
  microide::editor::FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "fold gutter renderer test should compute fold ranges");

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 72.0f};
  microide::editor::EditorViewModel view_model;
  view_model.fold_gutter_marks.push_back(
      microide::editor::FoldGutterMark{.line_index = 0, .visual_row_index = 0, .collapsed = false});

  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "", std::nullopt,
                  std::nullopt, {}, &view_model, false, false, false, &folding_model);

  const auto& marks = renderer.last_fold_gutter_marks();
  Expect(marks.size() == 1, "renderer should cache one expanded fold gutter mark");
  Expect(marks.front().line_index == 0 && marks.front().visual_row_index == 0,
         "expanded fold gutter mark should target the opener row");
  Expect(renderer.last_fold_gutter_marks().size() == 1 &&
             !renderer.last_fold_gutter_marks().front().collapsed,
         "renderer should cache one expanded fold gutter mark for the visible opener row");
}

void TestEditorViewRendererFoldGutterMarkerTracksCollapsedState() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 72);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};
  theme.line_number = SDL_Color{0xc8, 0xd2, 0xe6, 0xff};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("void f() {\n  body();\n}\n", "/tmp/editor-fold-gutter-collapse.cpp");
  microide::editor::FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "collapsed fold gutter renderer test should compute fold ranges");
  Expect(folding_model.Collapse(0),
         "collapsed fold gutter renderer test should collapse the fold");
  viewport.SetFoldingModel(&folding_model);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 72.0f};
  microide::editor::EditorViewModel view_model;
  view_model.fold_gutter_marks.push_back(
      microide::editor::FoldGutterMark{.line_index = 0, .visual_row_index = 0, .collapsed = true});

  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "", std::nullopt,
                  std::nullopt, {}, &view_model, false, false, false, &folding_model);

  Expect(renderer.last_fold_gutter_marks().size() == 1 &&
             renderer.last_fold_gutter_marks().front().collapsed,
         "renderer should cache one collapsed fold gutter mark for the visible opener row");
}

void TestEditorViewRendererBracketMatchCacheReusesAcrossFramesWithoutChange() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 7);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/true);
  Expect(renderer.bracket_match_cache_misses() == 1,
         "first frame with bracket-match enabled should populate the cache");
  Expect(renderer.last_bracket_match_pair().has_value(),
         "first frame should compute and cache a bracket-match pair");
  Expect(renderer.last_bracket_match_pair()->open_line == 0 &&
             renderer.last_bracket_match_pair()->open_column == 7,
         "cached pair should reflect the brace at line 0 col 7");

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/true);
  Expect(renderer.bracket_match_cache_hits() == 1,
         "second frame with unchanged caret/layout should reuse the cached pair");
  Expect(renderer.bracket_match_cache_misses() == 1,
         "second frame should not recompute the bracket-match pair");
}

void TestEditorViewRendererBracketMatchCacheInvalidatesOnCaretMove() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 7);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/true);
  Expect(renderer.bracket_match_cache_misses() == 1,
         "first frame should compute the pair");

  viewport.MoveCursorTo(0, 0);
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/true);
  Expect(renderer.bracket_match_cache_misses() == 2,
         "moving the caret should invalidate the cache and force a fresh compute");
  Expect(!renderer.last_bracket_match_pair().has_value(),
         "no bracket adjacent to the caret at line 0 col 0 should yield no pair");
}

void TestEditorViewRendererBracketMatchCacheClearsWhenToggleOff() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 7);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/true);
  Expect(renderer.last_bracket_match_pair().has_value(),
         "toggle-on frame should compute a bracket-match pair");

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/false);
  Expect(!renderer.last_bracket_match_pair().has_value(),
         "toggle-off frame should clear the cached pair so no paint occurs");
  Expect(renderer.bracket_match_cache_misses() == 1,
         "toggle-off frame should not run a fresh bracket-match compute");
  Expect(renderer.bracket_match_cache_hits() == 0,
         "toggle-off frame should not record a cache hit either");
}

void TestEditorViewRendererIndentGuidesCacheReusesAcrossUnchangedFrames() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (x) {\n    a();\n        b();\n    }\n}\n",
                        "/tmp/indent.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(2, 8);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/false,
                  /*indent_guides_enabled=*/true,
                  /*render_whitespace_enabled=*/false);
  Expect(renderer.indent_guides_cache_misses() == 1,
         "first frame with indent guides enabled should populate the cache");
  Expect(!renderer.last_indent_guide_runs().empty(),
         "first frame should compute at least one indent-guide run");

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr,
                  /*bracket_match_highlight_enabled=*/false,
                  /*indent_guides_enabled=*/true,
                  /*render_whitespace_enabled=*/false);
  Expect(renderer.indent_guides_cache_hits() == 1,
         "second frame with unchanged caret/scroll should reuse the indent-guide runs");
}

void TestEditorViewRendererIndentGuidesCacheInvalidatesOnCaretMove() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (x) {\n    a();\n        b();\n    }\n}\n",
                        "/tmp/indent.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(2, 8);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false, true, false);

  viewport.MoveCursorTo(0, 0);
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false, true, false);
  Expect(renderer.indent_guides_cache_misses() == 2,
         "moving the caret should invalidate the indent-guides cache");
}

void TestEditorViewRendererIndentGuidesClearOnToggleOff() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(220, 120);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (x) {\n    a();\n}\n", "/tmp/indent.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);

  const SDL_FRect rect{0.0f, 0.0f, 220.0f, 120.0f};
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false, true, false);
  Expect(!renderer.last_indent_guide_runs().empty(),
         "toggle-on frame should populate guide runs");

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false, false, false);
  Expect(renderer.last_indent_guide_runs().empty(),
         "toggle-off frame should clear cached guide runs so no paint occurs");
  Expect(renderer.indent_guides_cache_misses() == 1,
         "toggle-off frame should not run a fresh indent-guides compute");
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

void TestRenderViewModelBuilderOccurrencesScansVisibleLinesOnly() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::editor::TextViewport viewport;
  std::string content;
  for (int i = 0; i < 40; ++i) {
    content += "aaa\n";
  }
  content += "name\n";
  for (int i = 0; i < 40; ++i) {
    content += "bbb\n";
  }
  viewport.LoadContent(content, "/tmp/occ-viewport.txt");
  viewport.MoveCursorTo(40, 2, false);
  viewport.SetScrollLine(0);
  viewport.SetViewportSize(10, 120);

  const auto vm_disabled =
      builder.BuildEditorViewModel(viewport, 10, nullptr, false, false);
  Expect(vm_disabled.occurrence_ranges.empty(),
         "occurrence ranges should stay empty when highlighting is disabled");

  const auto vm_top =
      builder.BuildEditorViewModel(viewport, 10, nullptr, true, false);
  Expect(vm_top.occurrence_ranges.empty(),
         "caret line outside the viewport should yield no in-viewport matches");

  viewport.SetScrollLine(35);
  const auto vm_visible =
      builder.BuildEditorViewModel(viewport, 10, nullptr, true, false);
  Expect(!vm_visible.occurrence_ranges.empty(),
         "scrolling the seeded line into view should surface occurrence ranges");
  std::size_t primary = 0;
  for (const auto& occ : vm_visible.occurrence_ranges) {
    if (occ.line_index == 40 && occ.start_column == 0 && occ.end_column == 4 &&
        occ.is_primary_seed) {
      ++primary;
    }
  }
  Expect(primary == 1, "exactly one primary occurrence should match the seed span");
}

void TestRenderViewModelBuilderOccurrencesMatchWordInstancesInView() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::editor::TextViewport viewport;
  viewport.LoadContent("name bob name\nother\n", "/tmp/occ-double.txt");
  viewport.MoveCursorTo(0, 2, false);
  viewport.SetScrollLine(0);
  viewport.SetViewportSize(8, 120);
  const auto vm = builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  Expect(vm.occurrence_ranges.size() == 2,
         "visible logical line should surface every viewport-visible textual match");
}

void TestRenderViewModelBuilderOccurrenceSeedAndScanCachesHitOnStableFrames() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::editor::TextViewport viewport;

  std::string content;
  for (int i = 0; i < 20; ++i) {
    content += "aaa\n";
  }
  content += "name bob name\n";
  for (int i = 0; i < 30; ++i) {
    content += "zzz\n";
  }
  viewport.LoadContent(content, "/tmp/occ-cache-hit.txt");
  viewport.SetViewportSize(8, 120);
  viewport.MoveCursorTo(20, 2, false);

  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();
  const auto first_vm = builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  const auto second_vm = builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  Expect(!first_vm.occurrence_ranges.empty() &&
             second_vm.occurrence_ranges.size() == first_vm.occurrence_ranges.size(),
         "cached builds should keep the same visible occurrence projections");
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() == 1,
         "first frame should refill the occurrence seed detection cache exactly once");
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheHitsForTesting() == 1,
         "identical successive frames should hit the occurrence seed detection cache once");
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceScanCacheMissesForTesting() == 1,
         "first frame should refill the viewport occurrence scan once");
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceScanCacheHitsForTesting() == 1,
         "stable scroll + visible-rows count should reuse the scanned occurrence ranges");

  viewport.ScrollVertical(4);
  (void)builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceScanCacheMissesForTesting() == 2,
         "scroll changes must rebuild the occurrence scan even when caret is stable");
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheHitsForTesting() == 2,
         "seed detection should remain cached while the caret stays put");

  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();
  viewport.MoveCursorTo(20, 11, false);
  (void)builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  (void)builder.BuildEditorViewModel(viewport, 8, nullptr, true, false);
  Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() == 1 &&
             microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheHitsForTesting() == 1,
         "caret moves that change the seed word should refill once then hit");
}

void TestParseStickyScrollMaxDepthSettingClamp() {
  using microide::workspace::ParseStickyScrollMaxDepthSetting;
  Expect(ParseStickyScrollMaxDepthSetting(std::nullopt) == 3,
         "unset sticky depth defaults to three");
  Expect(ParseStickyScrollMaxDepthSetting(std::string()) == 3,
         "empty sticky depth defaults to three");
  Expect(ParseStickyScrollMaxDepthSetting(std::string("8")) == 8, "Eight should clamp to eight");
  Expect(ParseStickyScrollMaxDepthSetting(std::string("0")) == 1, "Too-small values clamp up");
  Expect(ParseStickyScrollMaxDepthSetting(std::string("99")) == 8,
         "Too-large values clamp down");
  // The previous implementation used try/catch around std::stol. The util::ParseInt rewrite must
  // preserve the "garbage input falls back to default" behavior without exceptions.
  Expect(ParseStickyScrollMaxDepthSetting(std::string("abc")) == 3,
         "non-numeric setting should fall back to the default sticky depth");
  Expect(ParseStickyScrollMaxDepthSetting(std::string("3xyz")) == 3,
         "trailing garbage should fall back to the default sticky depth");
}

void TestComputeStickyScrollLinesRespectsNestingAndDepth() {
  using microide::editor::FoldingModel;
  using microide::workspace::ComputeStickyScrollLinesUncached;
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = false;
  options.tab_size = 4;
  FoldingModel model;
  const std::vector<std::string> lines = {
      "void a() {",
      "  void b() {",
      "    body();",
      "  }",
      "}",
  };
  Expect(model.Compute(lines, options), "fold model should compute");
  microide::editor::TextViewport viewport;
  viewport.LoadContent(
      "void a() {\n"
      "  void b() {\n"
      "    body();\n"
      "  }\n"
      "}\n",
      "/tmp/sticky-nested.cpp");
  // Narrow height so ClampScrollState does not clamp scroll_line to zero when the
  // buffer fits wholly in view (sticky uses the first visible logical line).
  viewport.SetViewportSize(3, 120);
  viewport.SetScrollLine(2);  // first visible row is logical line 2 ("    body();")
  std::vector<std::size_t> sticky;
  ComputeStickyScrollLinesUncached(viewport, &model, true, 3, sticky);
  Expect(sticky.size() == 2 && sticky[0] == 0 && sticky[1] == 1,
         "sticky band should list outer then inner opener lines for the nested block");

  std::vector<std::size_t> depth1;
  ComputeStickyScrollLinesUncached(viewport, &model, true, 1, depth1);
  Expect(depth1.size() == 1 && depth1[0] == 1,
         "depth cap one should keep only the innermost enclosing opener");
}

void TestRenderViewModelBuilderStickyScrollCacheHitsUntilScrollOrFoldRevision() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  using microide::editor::FoldingModel;
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = false;
  options.tab_size = 4;
  FoldingModel model;
  const std::vector<std::string> lines = {
      "void a() {",
      "  void b() {",
      "    body();",
      "  }",
      "}",
  };
  Expect(model.Compute(lines, options), "fold model should compute");
  microide::editor::TextViewport viewport;
  viewport.LoadContent(
      "void a() {\n"
      "  void b() {\n"
      "    body();\n"
      "  }\n"
      "}\n",
      "/tmp/sticky-cache.cpp");
  viewport.SetViewportSize(3, 120);
  viewport.SetScrollLine(2);

  microide::workspace::RenderViewModelBuilder::ResetStickyScrollCacheForTesting();
  (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, true, 3);
  (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, true, 3);
  Expect(microide::workspace::RenderViewModelBuilder::StickyScrollCacheMissesForTesting() == 1,
         "first sticky build should miss the cache once");
  Expect(microide::workspace::RenderViewModelBuilder::StickyScrollCacheHitsForTesting() == 1,
         "unchanged scroll and fold revision should hit the sticky cache");

  viewport.SetScrollLine(3);
  (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, true, 3);
  Expect(microide::workspace::RenderViewModelBuilder::StickyScrollCacheMissesForTesting() == 2,
         "scroll change should invalidate the sticky cache");

  microide::workspace::RenderViewModelBuilder::ResetStickyScrollCacheForTesting();
  viewport.SetScrollLine(2);
  (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, false, 3);
  const auto vm_off = builder.BuildEditorViewModel(viewport, 8, &model, false, false, false, 3);
  Expect(vm_off.sticky_lines.empty(),
         "sticky scroll disabled should leave sticky_lines empty every frame");
}

void TestEditorEssentialsDisablingLayersClearsRendererCaches() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(360, 220);

  microide::render::TextRenderer text_renderer;
  TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  microide::editor::TextViewport viewport;
  viewport.LoadContent(
      "void a() {\n"
      "  void b() {\n"
      "    if (true) {\n"
      "      hello hello;\n"
      "      hello hello;\n"
      "      hello hello;\n"
      "      hello hello;\n"
      "    }\n"
      "    hello hello;\n"
      "    hello hello;\n"
      "    hello hello;\n"
      "  }\n"
      "}\n",
      "/tmp/essentials-cap-toggle.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.SetViewportSize(8, 120);
  viewport.MoveCursorTo(3, 6);
  viewport.SetScrollLine(2);

  microide::editor::FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "essentials toggle fixture should compute folds");

  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  const auto vm_on = builder.BuildEditorViewModel(viewport, 8, &folding_model, true, false, true, 3,
                                                   true);
  Expect(!vm_on.fold_gutter_marks.empty(),
         "fixture should emit fold gutter marks for visible openers");
  Expect(!vm_on.occurrence_ranges.empty(),
         "fixture should emit occurrences for the repeated word under the caret");
  Expect(!vm_on.sticky_lines.empty(),
         "fixture should emit sticky ancestors while scrolled inside nested folds");
  Expect(!vm_on.whitespace_glyph_runs.empty(),
         "fixture should emit whitespace glyphs when the VM builder enables them");

  const SDL_FRect rect{0.0f, 0.0f, 360.0f, 220.0f};
  microide::editor::EditorViewRenderer renderer;
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "hello",
                  std::nullopt, std::nullopt, {}, &vm_on,
                  /*bracket_match_highlight_enabled=*/true,
                  /*indent_guides_enabled=*/true,
                  /*render_whitespace_enabled=*/true,
                  &folding_model);
  Expect(!renderer.last_fold_gutter_marks().empty(),
         "enabled render pass should mirror fold gutter marks");

  microide::editor::EditorViewModel vm_empty{};
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "", std::nullopt,
                  std::nullopt, {}, &vm_empty,
                  /*bracket_match_highlight_enabled=*/false,
                  /*indent_guides_enabled=*/false,
                  /*render_whitespace_enabled=*/false,
                  nullptr);

  Expect(renderer.last_fold_gutter_marks().empty(),
         "all-off render should leave no cached fold gutter marks");
  Expect(!renderer.last_bracket_match_pair().has_value(),
         "bracket-match off should clear the cached pair");
  Expect(renderer.last_indent_guide_runs().empty(),
         "indent guides off should clear cached guide runs");
}

void TestDeviceAlignedOriginSnapsToPhysicalPixelGrid() {
  using microide::render::DeviceAlignedOrigin;

  // Integer scales are an exact no-op: glyph origins are already grid-aligned.
  Expect(DeviceAlignedOrigin(18.0f, 1.0f) == 18.0f,
         "scale 1.0 must leave the origin unchanged");
  Expect(DeviceAlignedOrigin(123.0f, 2.0f) == 123.0f,
         "integer scale must leave an aligned origin unchanged");
  Expect(DeviceAlignedOrigin(0.0f, 1.25f) == 0.0f, "zero origin stays at zero");

  // Non-positive / non-finite scales fall back to the input untouched.
  Expect(DeviceAlignedOrigin(17.3f, 0.0f) == 17.3f,
         "non-positive scale must not divide by zero");

  // Under a fractional scale the snapped origin must map onto a whole physical
  // pixel: result * scale is integral. These are the exact overlay offsets that
  // smeared before the fix (content_x=+18, list_top=+10 at 125% display scale).
  const float scales[] = {1.25f, 1.5f, 1.75f, 2.5f};
  const float origins[] = {10.0f, 18.0f, 26.0f, 134.5f, 207.0f};
  for (const float scale : scales) {
    for (const float origin : origins) {
      const float aligned = DeviceAlignedOrigin(origin, scale);
      const float physical = aligned * scale;
      Expect(std::fabs(physical - std::round(physical)) < 1e-3f,
             "snapped origin must land on an integer physical pixel");
      // Snapping never moves the origin by more than half a physical pixel.
      Expect(std::fabs(aligned - origin) * scale <= 0.5f + 1e-3f,
             "snap must stay within half a physical pixel of the request");
    }
  }
}

void TestTextLayoutIdentifierRangeAt() {
  using microide::editor::TextLayout;

  // Cursor anywhere inside an identifier yields the full word range.
  const std::string line = "  int count_2 = other->x;";
  const std::size_t c = line.find("count_2");
  for (std::size_t off = 0; off < std::string("count_2").size(); ++off) {
    const auto r = TextLayout::IdentifierRangeAt(line, c + off);
    Expect(!r.empty() && r.start == c && r.end == c + 7,
           "any byte inside count_2 resolves to the whole identifier");
  }

  // Non-identifier bytes (space, punctuation, past end-of-line) resolve to empty.
  Expect(TextLayout::IdentifierRangeAt(line, 0).empty(), "leading space is not an identifier");
  Expect(TextLayout::IdentifierRangeAt(line, line.find('=')).empty(),
         "operator byte is not an identifier");
  Expect(TextLayout::IdentifierRangeAt(line, line.size()).empty(),
         "past end-of-line is empty");
  Expect(TextLayout::IdentifierRangeAt("", 0).empty(), "empty line is empty");

  // Member access does not merge across `->`/`.` (identifier-only, Phase 5 contract).
  const std::size_t x = line.find("->x") + 2;
  const auto rx = TextLayout::IdentifierRangeAt(line, x);
  Expect(!rx.empty() && line.substr(rx.start, rx.end - rx.start) == "x",
         "member access stops at the operator");

  // Tabs/leading indent do not affect byte-offset resolution.
  const std::string tabbed = "\t\tvalue";
  const auto rt = TextLayout::IdentifierRangeAt(tabbed, 3);
  Expect(!rt.empty() && rt.start == 2 && rt.end == tabbed.size(),
         "identifier after tabs resolves by byte offset");
}

// The render-whitespace markers have two producers: the fast view-model path
// (CSR-indexed whitespace_glyph_runs) and the text-iteration fallback used when
// no view model is supplied. Both now build their fill rects through the shared
// PushWhitespaceMarker helper, so for a fully-visible, non-wrapped buffer they
// must paint pixel-for-pixel identically. This guards the dedup that unified them.
void TestEditorViewRendererWhitespaceMarkersMatchAcrossViewModelAndFallbackPaths() {
  EnsureDummySdlVideo();
  const SDL_Color background{0x08, 0x08, 0x08, 0xff};
  const SDL_FRect rect{0.0f, 0.0f, 320.0f, 96.0f};

  microide::editor::TextViewport viewport;
  viewport.LoadContent("\tint x = 1;\n  two  spaces\n", "/tmp/ws-parity.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.SetViewportSize(4, 80);
  viewport.SetScrollLine(0);

  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  theme.editor_background = background;
  theme.gutter_background = SDL_Color{0x12, 0x12, 0x12, 0xff};

  // View-model path: build the CSR whitespace glyph runs the fast path iterates.
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();
  microide::workspace::RenderViewModelBuilder::ResetStickyScrollCacheForTesting();
  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(vm, viewport, /*visible_rows=*/4, /*folding_model=*/nullptr,
                                   /*occurrences_highlight_enabled=*/false,
                                   /*occurrences_case_sensitive=*/false,
                                   /*sticky_scroll_enabled=*/false,
                                   /*sticky_max_depth=*/3,
                                   /*render_whitespace_enabled=*/true);
  Expect(!vm.whitespace_glyph_runs.empty(),
         "whitespace parity test should produce glyph runs to exercise the view-model path");

  const auto render_to = [&](SoftwareCanvas& canvas, const microide::editor::EditorViewModel* view_model) {
    microide::render::TextRenderer text_renderer;
    TextRendererTestAccess::SetBackend(text_renderer, std::make_unique<CountingTextBackend>());
    Expect(SDL_SetRenderDrawColor(canvas.renderer(), background.r, background.g, background.b,
                                  background.a),
           "whitespace parity test should set the canvas background");
    Expect(SDL_RenderClear(canvas.renderer()), "whitespace parity test should clear the canvas");
    microide::editor::EditorViewRenderer renderer;
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, /*draw_caret=*/false,
                    /*search_query=*/{}, /*active_search_match=*/std::nullopt,
                    /*blame_overlay=*/std::nullopt, /*diagnostics=*/{}, view_model,
                    /*bracket_match_highlight_enabled=*/false, /*indent_guides_enabled=*/false,
                    /*render_whitespace_enabled=*/true);
  };

  SoftwareCanvas vm_canvas(320, 96);
  SoftwareCanvas fallback_canvas(320, 96);
  render_to(vm_canvas, &vm);
  render_to(fallback_canvas, /*view_model=*/nullptr);

  SDL_Surface* vm_pixels = SDL_RenderReadPixels(vm_canvas.renderer(), nullptr);
  SDL_Surface* fallback_pixels = SDL_RenderReadPixels(fallback_canvas.renderer(), nullptr);
  Expect(vm_pixels != nullptr && fallback_pixels != nullptr,
         "whitespace parity test should read software pixels");
  Expect(NonBackgroundBounds(vm_pixels, background).valid(),
         "whitespace parity test should paint visible content");
  Expect(CountPixelDifferences(vm_pixels, fallback_pixels) == 0,
         "view-model whitespace path and text-iteration fallback must paint identical pixels");
  SDL_DestroySurface(vm_pixels);
  SDL_DestroySurface(fallback_pixels);
}

}  // namespace

void RegisterTextRendererTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextLayout identifier range at cursor", TestTextLayoutIdentifierRangeAt);
  AddTest(tests,
          "TextRenderer decorated grid paints row fills and underlines separately",
          TestDecoratedTextGridRendererPaintsRowFillAndUnderline);
  AddTest(tests,
          "TextRenderer editor view composes selected rows and inline highlights through decorated rows",
          TestEditorViewRendererPaintsSelectedRowsAndInlineHighlightsThroughDecoratedGrid);
  AddTest(tests,
          "TextRenderer editor view uses wrapped rows and suppresses continuation gutter numbers",
          TestEditorViewRendererUsesWrappedRowsAndSuppressesContinuationGutterNumbers);
  AddTest(tests,
          "TextRenderer editor view search highlights track horizontal scroll",
          TestEditorViewRendererSearchHighlightsTrackHorizontalScroll);
#ifndef NDEBUG
  AddTest(tests,
          "TextRenderer editor view reuses wrap cache across frames",
          TestEditorViewRendererReusesWrapCacheAcrossFrames);
#endif
  AddTest(tests,
          "TextRenderer editor view advances past wrapped empty lines",
          TestEditorViewRendererAdvancesPastWrappedEmptyLines);
  AddTest(tests,
          "TextRenderer editor view clips wrapped selections per row",
          TestEditorViewRendererClipsWrappedSelectionsPerRow);
  AddTest(tests,
          "TextRenderer editor view paints diagnostic underlines",
          TestEditorViewRendererPaintsDiagnosticUnderlines);
  AddTest(tests,
          "TextRenderer editor view whitespace markers match across view-model and fallback paths",
          TestEditorViewRendererWhitespaceMarkersMatchAcrossViewModelAndFallbackPaths);
  AddTest(tests,
          "TextRenderer diagnostic line severity prefers the most severe match",
          TestHighestDiagnosticSeverityForLinePrefersTheMostSevereMatch);
  AddTest(tests,
          "TextRenderer editor view paints diagnostic gutter markers",
          TestEditorViewRendererPaintsDiagnosticGutterMarkers);
  AddTest(tests,
          "TextRenderer editor view paints fold gutter markers",
          TestEditorViewRendererPaintsFoldGutterMarkers);
  AddTest(tests,
          "TextRenderer editor view fold gutter markers track collapsed state",
          TestEditorViewRendererFoldGutterMarkerTracksCollapsedState);
  AddTest(tests,
          "TextRenderer editor view bracket-match cache reuses pair across unchanged frames",
          TestEditorViewRendererBracketMatchCacheReusesAcrossFramesWithoutChange);
  AddTest(tests,
          "TextRenderer editor view bracket-match cache invalidates on caret move",
          TestEditorViewRendererBracketMatchCacheInvalidatesOnCaretMove);
  AddTest(tests,
          "TextRenderer editor view bracket-match cache clears when highlight toggle is off",
          TestEditorViewRendererBracketMatchCacheClearsWhenToggleOff);
  AddTest(tests,
          "TextRenderer editor view indent-guides cache reuses runs across unchanged frames",
          TestEditorViewRendererIndentGuidesCacheReusesAcrossUnchangedFrames);
  AddTest(tests,
          "TextRenderer editor view indent-guides cache invalidates on caret move",
          TestEditorViewRendererIndentGuidesCacheInvalidatesOnCaretMove);
  AddTest(tests,
          "TextRenderer editor view indent-guides clear cached runs when toggle is off",
          TestEditorViewRendererIndentGuidesClearOnToggleOff);
  AddTest(tests,
          "TextRenderer editor presentation layers clear caches when toggles turn off",
          TestEditorEssentialsDisablingLayersClearsRendererCaches);
  AddTest(tests,
          "TextRenderer render view model occurrence scan stays viewport-bounded",
          TestRenderViewModelBuilderOccurrencesScansVisibleLinesOnly);
  AddTest(tests,
          "TextRenderer render view model occurrence scan lists every visible match on a line",
          TestRenderViewModelBuilderOccurrencesMatchWordInstancesInView);
  AddTest(tests,
          "TextRenderer render view model occurrence scan cache hits stable frames then invalidates",
          TestRenderViewModelBuilderOccurrenceSeedAndScanCachesHitOnStableFrames);
  AddTest(tests,
          "TextRenderer sticky-scroll max-depth setting parses into 1..8 range",
          TestParseStickyScrollMaxDepthSettingClamp);
  AddTest(tests,
          "TextRenderer sticky-scroll unresolved openers nest with innermost capped by depth",
          TestComputeStickyScrollLinesRespectsNestingAndDepth);
  AddTest(tests,
          "TextRenderer render view model sticky-scroll cache hits until scroll moves",
          TestRenderViewModelBuilderStickyScrollCacheHitsUntilScrollOrFoldRevision);
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
  AddTest(tests,
          "TextRenderer device-aligned origin snaps text to the physical pixel grid",
          TestDeviceAlignedOriginSnapsToPhysicalPixelGrid);
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
          "TextRenderer SDL_ttf SetFontPointSize resizes glyph metrics",
          TestSdlTtfSetFontPointSizeResizesGlyphMetrics);
  AddTest(tests,
          "TextRenderer SDL_ttf SetFontFamily resilient to unresolved family",
          TestSdlTtfSetFontFamilyResilientToUnresolved);
  AddTest(tests,
          "TextRenderer SDL_ttf ASCII prefix glyph stays fixed when appending matches",
          TestSdlTtfAsciiPrefixGlyphStaysFixedWhenAppendingMatches);
  AddTest(tests,
          "TextRenderer SDL_ttf blended text keeps transparent corners",
          TestSdlTtfBlendedTextKeepsTransparentCorners);
  AddTest(tests,
          "AsciiGlyphAtlas tinted blit matches direct per-color rendering",
          TestAsciiGlyphAtlasMatchesPerColorRendering);
  AddTest(tests,
          "AsciiGlyphAtlas covers the printable ASCII range",
          TestAsciiGlyphAtlasCoversPrintableRange);
#endif
}

}  // namespace microide::tests
