#include "TestSupport.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

#include "app/DirtyRegionPolicy.h"

namespace microide::tests {
namespace {

using microide::app::AnalyzeDirtyRegions;
using microide::app::DirtyRegionAnalysis;
using microide::app::ShouldPromotePartialFrameToFull;

void ExpectRectEquals(const SDL_Rect& actual,
                      const SDL_Rect& expected,
                      std::string_view context) {
  Expect(actual.x == expected.x, context);
  Expect(actual.y == expected.y, context);
  Expect(actual.w == expected.w, context);
  Expect(actual.h == expected.h, context);
}

void TestDirtyRegionsMergeOverlappingRects() {
  const DirtyRegionAnalysis analysis =
      AnalyzeDirtyRegions({SDL_FRect{.x = 0.0f, .y = 0.0f, .w = 10.0f, .h = 10.0f},
                           SDL_FRect{.x = 5.0f, .y = 5.0f, .w = 10.0f, .h = 10.0f}},
                          {}, 20, 20);

  Expect(analysis.clipped_rect_count == 2,
         "two clipped dirty rects should feed the coalescing pass");
  Expect(analysis.merged_clip_rects.size() == 1,
         "overlapping dirty rects should merge into one clip rect");
  ExpectRectEquals(analysis.merged_clip_rects.front(),
                   SDL_Rect{.x = 0, .y = 0, .w = 15, .h = 15},
                   "merged clip rect should span the full overlap union");
  Expect(std::fabs(analysis.coverage - 0.5625f) < 0.001f,
         "coverage should come from the merged clip area");
}

void TestDirtyRegionsMergeTouchingRects() {
  const DirtyRegionAnalysis analysis =
      AnalyzeDirtyRegions({SDL_FRect{.x = 10.0f, .y = 4.0f, .w = 8.0f, .h = 6.0f},
                           SDL_FRect{.x = 18.0f, .y = 4.0f, .w = 8.0f, .h = 6.0f}},
                          {}, 40, 20);

  Expect(analysis.merged_clip_rects.size() == 1,
         "touching rects should coalesce so partial replay does not redraw them separately");
  ExpectRectEquals(analysis.merged_clip_rects.front(),
                   SDL_Rect{.x = 10, .y = 4, .w = 16, .h = 6},
                   "merged clip rect should cover the touching rect pair");
}

void TestDirtyCoverageStaysBounded() {
  const DirtyRegionAnalysis analysis =
      AnalyzeDirtyRegions({SDL_FRect{.x = 0.0f, .y = 0.0f, .w = 100.0f, .h = 100.0f},
                           SDL_FRect{.x = 10.0f, .y = 10.0f, .w = 80.0f, .h = 80.0f},
                           SDL_FRect{.x = 20.0f, .y = 20.0f, .w = 60.0f, .h = 60.0f}},
                          {}, 100, 100);

  Expect(analysis.merged_clip_rects.size() == 1,
         "nested dirty rects should collapse to one full-scene clip");
  Expect(std::fabs(analysis.coverage - 1.0f) < 0.001f,
         "coalesced coverage should be capped at 100 percent");
}

void TestPromotionUsesCoalescedClipCount() {
  const DirtyRegionAnalysis coalesced_single_region =
      AnalyzeDirtyRegions({SDL_FRect{.x = 0.0f, .y = 60.0f, .w = 120.0f, .h = 40.0f},
                           SDL_FRect{.x = 0.0f, .y = 58.0f, .w = 120.0f, .h = 42.0f},
                           SDL_FRect{.x = 0.0f, .y = 56.0f, .w = 120.0f, .h = 44.0f},
                           SDL_FRect{.x = 0.0f, .y = 54.0f, .w = 120.0f, .h = 46.0f},
                           SDL_FRect{.x = 0.0f, .y = 52.0f, .w = 120.0f, .h = 48.0f},
                           SDL_FRect{.x = 0.0f, .y = 50.0f, .w = 120.0f, .h = 50.0f},
                           SDL_FRect{.x = 0.0f, .y = 48.0f, .w = 120.0f, .h = 52.0f},
                           SDL_FRect{.x = 0.0f, .y = 46.0f, .w = 120.0f, .h = 54.0f}},
                          {}, 120, 100);
  Expect(coalesced_single_region.clipped_rect_count == 8,
         "the raw dirty storm should still be visible before coalescing");
  Expect(coalesced_single_region.merged_clip_rects.size() == 1,
         "overlapping resize dirty rects should collapse to one clip region");
  Expect(!ShouldPromotePartialFrameToFull(coalesced_single_region),
         "one coalesced clip region should stay partial even if many raw dirty rects accumulated");

  const DirtyRegionAnalysis fragmented_regions =
      AnalyzeDirtyRegions({SDL_FRect{.x = 0.0f, .y = 0.0f, .w = 34.0f, .h = 100.0f},
                           SDL_FRect{.x = 43.0f, .y = 0.0f, .w = 34.0f, .h = 100.0f},
                           SDL_FRect{.x = 86.0f, .y = 0.0f, .w = 34.0f, .h = 100.0f}},
                          {}, 120, 100);
  Expect(fragmented_regions.merged_clip_rects.size() == 3,
         "separated broad dirty bands should remain fragmented after coalescing");
  Expect(fragmented_regions.coverage > 0.8f,
         "wide fragmented bands should still report high coalesced coverage");
  Expect(ShouldPromotePartialFrameToFull(fragmented_regions),
         "wide fragmented redraws should still promote to full redraw");
}

// TD-2026-07-16-48: non-finite, negative-size, or huge float dirty rects must be
// rejected/clamped before the float->int cast (which is UB for NaN/inf/out-of-range).
// A valid rect in the same batch must still produce its clip.
void TestDirtyRegionsRejectNonFiniteAndHugeInput() {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const DirtyRegionAnalysis analysis = AnalyzeDirtyRegions(
      {
          SDL_FRect{.x = nan, .y = 0.0f, .w = 10.0f, .h = 10.0f},   // NaN origin
          SDL_FRect{.x = 0.0f, .y = 0.0f, .w = inf, .h = 10.0f},    // infinite width
          SDL_FRect{.x = 0.0f, .y = 0.0f, .w = -5.0f, .h = 10.0f},  // negative width
          SDL_FRect{.x = 1e30f, .y = 1e30f, .w = 1e30f, .h = 1e30f},  // huge finite
          SDL_FRect{.x = 2.0f, .y = 2.0f, .w = 4.0f, .h = 4.0f},    // valid
      },
      {}, 40, 40);

  Expect(analysis.clipped_rect_count == 1,
         "only the single valid dirty rect should survive validation");
  Expect(analysis.merged_clip_rects.size() == 1, "the valid rect produces one clip");
  const SDL_Rect& clip = analysis.merged_clip_rects.front();
  Expect(clip.x >= 0 && clip.y >= 0 && clip.w > 0 && clip.h > 0 && clip.x + clip.w <= 40 &&
             clip.y + clip.h <= 40,
         "the surviving clip is within surface bounds with a positive size");
}

}  // namespace

void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirtyRegionPolicy/RejectNonFiniteAndHugeInput",
          TestDirtyRegionsRejectNonFiniteAndHugeInput);
  AddTest(tests, "DirtyRegionPolicy/MergesOverlappingRects",
          TestDirtyRegionsMergeOverlappingRects);
  AddTest(tests, "DirtyRegionPolicy/MergesTouchingRects",
          TestDirtyRegionsMergeTouchingRects);
  AddTest(tests, "DirtyRegionPolicy/CoverageStaysBounded",
          TestDirtyCoverageStaysBounded);
  AddTest(tests, "DirtyRegionPolicy/PromotionUsesCoalescedClipCount",
          TestPromotionUsesCoalescedClipCount);
}

}  // namespace microide::tests
