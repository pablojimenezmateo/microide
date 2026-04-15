#include "TestSupport.h"

#include "render/TextRenderer.h"

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
}

}  // namespace microide::tests
