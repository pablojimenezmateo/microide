#include "TestSupport.h"

#include "app/ApplicationPresentationCache.h"

namespace microide::tests {
namespace {

using microide::app::CanReuseCachedPresentationState;
using WindowPresentationState = microide::workspace::WorkspaceShell::WindowPresentationState;

void TestPresentationCacheReusedWhenUiScaleMatches() {
  const std::optional<WindowPresentationState> cached = WindowPresentationState{
      .logical_width = 1200,
      .logical_height = 800,
      .scale_x = 1.25f,
      .scale_y = 1.25f,
  };
  Expect(CanReuseCachedPresentationState(false, cached, 1.25f, 1.25f),
         "presentation cache should be reusable when the ui scale matches");
}

void TestPresentationCacheInvalidatedWhenUiScaleChanges() {
  const std::optional<WindowPresentationState> cached = WindowPresentationState{
      .logical_width = 1200,
      .logical_height = 800,
      .scale_x = 1.25f,
      .scale_y = 1.25f,
  };
  Expect(!CanReuseCachedPresentationState(false, cached, 1.0f, 1.25f),
         "presentation cache should be bypassed when the ui scale changes without a window event");
}

}  // namespace

void RegisterApplicationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Application/PresentationCacheReusedWhenUiScaleMatches",
          TestPresentationCacheReusedWhenUiScaleMatches);
  AddTest(tests, "Application/PresentationCacheInvalidatedWhenUiScaleChanges",
          TestPresentationCacheInvalidatedWhenUiScaleChanges);
}

}  // namespace microide::tests
