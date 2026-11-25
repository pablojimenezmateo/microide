#include "TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "app/DirtyRegionPolicy.h"
#include "render/DevicePixelSnap.h"

namespace microide::tests {
namespace {

using microide::app::AnalyzeDirtyRegions;
using microide::app::DeviceClipHint;
using microide::app::DirtyRegionAnalysis;
using microide::app::PadClipForPresentationScale;
using microide::app::ShouldPromotePartialFrameToFull;

bool DirtyPointCovered(const std::vector<SDL_Rect>& rects, int x, int y) {
  for (const SDL_Rect& rect : rects) {
    if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) {
      return true;
    }
  }
  return false;
}

void TestDirtyRegionMergeCoversEveryDamagedPixel() {
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  const render::TextClipPadding padding{
      .left = 2.0f, .right = 3.0f, .top = 1.0f, .bottom = 1.0f};

  std::uint64_t seed = 0x9B05688C2B3E6C1FULL;
  const auto next = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(seed >> 33);
  };

  std::size_t checked_points = 0;
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::vector<SDL_FRect> dirty;
    const std::size_t count = 1 + next() % 8;
    for (std::size_t i = 0; i < count; ++i) {
      const float x = static_cast<float>(static_cast<int>(next() % 500) - 50);
      const float y = static_cast<float>(static_cast<int>(next() % 400) - 50);
      const float w = static_cast<float>(next() % 120);
      const float h = static_cast<float>(next() % 90);
      dirty.push_back(SDL_FRect{x, y, w, h});
    }

    const app::DirtyRegionAnalysis analysis =
        app::AnalyzeDirtyRegions(dirty, padding, kWidth, kHeight);
    if (app::ShouldPromotePartialFrameToFull(analysis)) {
      continue;
    }

    for (const SDL_FRect& rect : dirty) {
      if (rect.w <= 0.0f || rect.h <= 0.0f) {
        continue;
      }
      const int left =
          std::max(0, static_cast<int>(std::floor(rect.x - padding.left)));
      const int top = std::max(0, static_cast<int>(std::floor(rect.y - padding.top)));
      const int right = std::min(
          kWidth, static_cast<int>(std::ceil(rect.x + rect.w + padding.right)));
      const int bottom = std::min(
          kHeight, static_cast<int>(std::ceil(rect.y + rect.h + padding.bottom)));
      for (int y = top; y < bottom; y += 3) {
        for (int x = left; x < right; x += 3) {
          Expect(DirtyPointCovered(analysis.merged_clip_rects, x, y),
                 "every padded, on-screen damaged pixel must be inside a merged clip rect");
          ++checked_points;
        }
      }
    }

    for (const SDL_Rect& rect : analysis.merged_clip_rects) {
      Expect(rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= kWidth &&
                 rect.y + rect.h <= kHeight,
             "a merged clip rect must stay inside the target");
      Expect(rect.w > 0 && rect.h > 0, "a merged clip rect must be non-empty");
    }
  }
  Expect(checked_points > 10000, "the sweep must actually have damaged pixels to check");
}

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

void TestDirtyRegionsRejectNonFiniteAndHugeInput() {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const DirtyRegionAnalysis analysis = AnalyzeDirtyRegions(
      {
          SDL_FRect{.x = nan, .y = 0.0f, .w = 10.0f, .h = 10.0f},
          SDL_FRect{.x = 0.0f, .y = 0.0f, .w = inf, .h = 10.0f},
          SDL_FRect{.x = 0.0f, .y = 0.0f, .w = -5.0f, .h = 10.0f},
          SDL_FRect{.x = 1e30f, .y = 1e30f, .w = 1e30f, .h = 1e30f},
          SDL_FRect{.x = 2.0f, .y = 2.0f, .w = 4.0f, .h = 4.0f},
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

void TestClipsCoverTheirDevicePixelsAtFractionalScales() {
  constexpr int kWidth = 1152;
  constexpr int kHeight = 720;
  for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f}) {
    const render::TextClipPadding padding = PadClipForPresentationScale({}, scale, scale);
    for (int x = 800; x < 840; ++x) {
      for (const int w : {1, 2, 8, 9}) {
        const SDL_FRect damaged{static_cast<float>(x), 676.0f, static_cast<float>(w), 18.0f};
        const DirtyRegionAnalysis analysis = AnalyzeDirtyRegions({damaged}, padding, kWidth, kHeight);
        Expect(analysis.merged_clip_rects.size() == 1, "one damaged rect should make one clip");
        if (analysis.merged_clip_rects.empty()) {
          return;
        }
        const SDL_Rect& clip = analysis.merged_clip_rects.front();
        const int device_x0 = static_cast<int>(std::floor(static_cast<float>(clip.x) * scale));
        const int device_x1 = device_x0 + static_cast<int>(std::ceil(static_cast<float>(clip.w) * scale));
        const int device_y0 = static_cast<int>(std::floor(static_cast<float>(clip.y) * scale));
        const int device_y1 = device_y0 + static_cast<int>(std::ceil(static_cast<float>(clip.h) * scale));
        const int fill_x0 = static_cast<int>(std::floor(damaged.x * scale));
        const int fill_x1 = static_cast<int>(std::ceil((damaged.x + damaged.w) * scale));
        const int fill_y0 = static_cast<int>(std::floor(damaged.y * scale));
        const int fill_y1 = static_cast<int>(std::ceil((damaged.y + damaged.h) * scale));
        if (fill_x0 < device_x0 || fill_x1 > device_x1 || fill_y0 < device_y0 || fill_y1 > device_y1) {
          Expect(false, "at scale " + std::to_string(scale) + " the clip of x=" + std::to_string(x) +
                            " w=" + std::to_string(w) + " covers device columns [" +
                            std::to_string(device_x0) + "," + std::to_string(device_x1) +
                            ") but the rect fills [" + std::to_string(fill_x0) + "," +
                            std::to_string(fill_x1) + ")");
          return;
        }
      }
    }
  }
}

void TestTheDirtyHintIsTheClipsDeviceExtent() {
  const SDL_Rect clip{0, 57, 289, 389};
  const SDL_FRect hint = DeviceClipHint(clip, 1.5f, 1.5f);
  const SDL_FRect editor{289.0f, 57.0f, 300.0f, 389.0f};
  Expect(hint.x + hint.w > editor.x, "the hint should reach into the editor's first device column, ends at " +
                                         std::to_string(hint.x + hint.w));
  const SDL_FRect same = DeviceClipHint(clip, 1.0f, 1.0f);
  Expect(same.x == 0.0f && same.y == 57.0f && same.w == 289.0f && same.h == 389.0f,
         "at scale 1 the hint is the clip");
  const SDL_FRect doubled = DeviceClipHint(clip, 2.0f, 2.0f);
  Expect(doubled.x == 0.0f && doubled.w == 289.0f && doubled.h == 389.0f, "at scale 2 the hint is the clip");
}


void TestAnalyzingIntoAKeptAnalysisReusesItsStorage() {
  using microide::app::AnalyzeDirtyRegions;
  using microide::app::DirtyRegionAnalysis;
  const microide::render::TextClipPadding padding{};
  std::vector<SDL_FRect> rows;
  for (int i = 0; i < 12; ++i) {
    rows.push_back(SDL_FRect{.x = 0.0f, .y = 40.0f * static_cast<float>(i), .w = 30.0f, .h = 10.0f});
  }
  DirtyRegionAnalysis kept;
  AnalyzeDirtyRegions(rows, padding, 800, 600, SDL_FPoint{1.0f, 1.0f}, kept);
  const DirtyRegionAnalysis fresh = AnalyzeDirtyRegions(rows, padding, 800, 600);
  Expect(kept.merged_clip_rects.size() == fresh.merged_clip_rects.size() &&
             kept.clipped_rect_count == fresh.clipped_rect_count && kept.coverage == fresh.coverage,
         "a kept analysis matches the by-value one");
  const SDL_Rect* storage = kept.merged_clip_rects.data();
  AnalyzeDirtyRegions(rows, padding, 800, 600, SDL_FPoint{1.0f, 1.0f}, kept);
  Expect(kept.merged_clip_rects.data() == storage, "the next frame reuses the clip list's storage");
  AnalyzeDirtyRegions({}, padding, 800, 600, SDL_FPoint{1.0f, 1.0f}, kept);
  Expect(kept.merged_clip_rects.empty() && kept.clipped_rect_count == 0 && kept.coverage == 0.0f,
         "a frame with no damage keeps nothing from the one before");
}

void TestADamageClipIsTheSharedSnapOfItsRect() {
  const microide::render::TextClipPadding padding{};
  for (const float scale : {1.25f, 1.5f, 1.75f}) {
    const SDL_FRect damaged{101.3f, 37.6f, 40.2f, 18.9f};
    const DirtyRegionAnalysis analysis =
        AnalyzeDirtyRegions({damaged}, padding, 2000, 2000, SDL_FPoint{scale, scale});
    const SDL_Rect expected = microide::render::SnapToDevicePixels(damaged, SDL_FPoint{scale, scale});
    Expect(analysis.merged_clip_rects.size() == 1, "one clip");
    const SDL_Rect& clip = analysis.merged_clip_rects.front();
    Expect(clip.x == expected.x && clip.y == expected.y && clip.w == expected.w && clip.h == expected.h,
           "the damage clip at scale " + std::to_string(scale) + " is " + std::to_string(clip.x) + "," +
               std::to_string(clip.y) + "," + std::to_string(clip.w) + "," + std::to_string(clip.h) +
               ", the shared snap is " + std::to_string(expected.x) + "," + std::to_string(expected.y) + "," +
               std::to_string(expected.w) + "," + std::to_string(expected.h));
  }
}
}

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
  AddTest(tests, "DirtyRegionPolicy/TheDirtyHintIsTheClipsDeviceExtent",
          TestTheDirtyHintIsTheClipsDeviceExtent);
  AddTest(tests, "DirtyRegionPolicy/ClipsCoverTheirDevicePixelsAtFractionalScales",
          TestClipsCoverTheirDevicePixelsAtFractionalScales);
  AddTest(tests, "DirtyRegionPolicy/MergeCoversEveryDamagedPixel",
          TestDirtyRegionMergeCoversEveryDamagedPixel);
  AddTest(tests, "DirtyRegionPolicy/ADamageClipIsTheSharedSnapOfItsRect", TestADamageClipIsTheSharedSnapOfItsRect);
  AddTest(tests, "DirtyRegionPolicy/AnalyzingIntoAKeptAnalysisReusesItsStorage",
          TestAnalyzingIntoAKeptAnalysisReusesItsStorage);
}

}
