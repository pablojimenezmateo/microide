#include "TestSupport.h"

#include <cmath>
#include <vector>

#include "workspace/TabStripAnimation.h"

namespace microide::tests {
namespace {

using workspace::AdvanceSlideOffsets;
using workspace::ComputeSlideTargetXs;
using workspace::SlideOffsetsMoving;
using workspace::SlideTab;

// Uniform strip: four tabs, width 100, gap 1, starting at x=0.
std::vector<SlideTab> UniformStrip() {
  return {
      SlideTab{.index = 0, .x = 0.0f, .width = 100.0f},
      SlideTab{.index = 1, .x = 101.0f, .width = 100.0f},
      SlideTab{.index = 2, .x = 202.0f, .width = 100.0f},
      SlideTab{.index = 3, .x = 303.0f, .width = 100.0f},
  };
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void TestSlideDragRightPullsNeighborLeftAndHoldsGap() {
  const auto tabs = UniformStrip();
  // Drag tab 1, hover the gap before tab 3 (insertion slot 3).
  const std::vector<float> t = ComputeSlideTargetXs(tabs, /*source=*/1, /*insertion=*/3,
                                                    /*ghost_width=*/100.0f, /*gap=*/1.0f);
  Expect(t.size() == 4, "one target per input tab");
  Expect(Near(t[0], 0.0f), "tab 0 stays at the front");
  Expect(Near(t[2], 101.0f), "tab 2 slides left into the vacated slot 1");
  Expect(Near(t[3], 303.0f), "tab 3 keeps its slot; the gap opens in front of it");
}

void TestSlideHoverHomeMovesNothing() {
  const auto tabs = UniformStrip();
  // Drag tab 1, hover its own slot (insertion slot 1): the gap stays home.
  const std::vector<float> t = ComputeSlideTargetXs(tabs, /*source=*/1, /*insertion=*/1,
                                                    /*ghost_width=*/100.0f, /*gap=*/1.0f);
  Expect(Near(t[0], 0.0f) && Near(t[2], 202.0f) && Near(t[3], 303.0f),
         "hovering the source slot leaves every neighbor at its base x");
}

void TestSlideDropPastEndClosesUpBehind() {
  const auto tabs = UniformStrip();
  // Drag tab 1, drop past the last tab (insertion slot 4).
  const std::vector<float> t = ComputeSlideTargetXs(tabs, /*source=*/1, /*insertion=*/4,
                                                    /*ghost_width=*/100.0f, /*gap=*/1.0f);
  Expect(Near(t[2], 101.0f) && Near(t[3], 202.0f),
         "tabs 2 and 3 slide left to close the vacated slot; the gap trails the strip");
}

void TestSlideDragLeftOpensGapAtFront() {
  const auto tabs = UniformStrip();
  // Drag tab 2 to the very front (insertion slot 0).
  const std::vector<float> t = ComputeSlideTargetXs(tabs, /*source=*/2, /*insertion=*/0,
                                                    /*ghost_width=*/100.0f, /*gap=*/1.0f);
  Expect(Near(t[0], 101.0f) && Near(t[1], 202.0f),
         "tabs 0 and 1 slide right by one tab to open a gap at the front");
  Expect(Near(t[3], 303.0f), "the trailing tab is unaffected");
}

void TestSlideVariableWidths() {
  const std::vector<SlideTab> tabs = {
      SlideTab{.index = 0, .x = 0.0f, .width = 132.0f},
      SlideTab{.index = 1, .x = 133.0f, .width = 200.0f},
      SlideTab{.index = 2, .x = 334.0f, .width = 150.0f},
  };
  // Drag tab 0, hover the gap before tab 2 (insertion slot 2).
  const std::vector<float> t = ComputeSlideTargetXs(tabs, /*source=*/0, /*insertion=*/2,
                                                    /*ghost_width=*/132.0f, /*gap=*/1.0f);
  Expect(Near(t[1], 0.0f), "tab 1 slides to the front to fill tab 0's vacated slot");
  Expect(Near(t[2], 334.0f),
         "the gap (ghost width 132 + gap) opens in front of tab 2, leaving it in place");
}

void TestSlideEmptyStripIsSafe() {
  const std::vector<SlideTab> empty;
  const std::vector<float> t = ComputeSlideTargetXs(empty, 0, 0, 100.0f, 1.0f);
  Expect(t.empty(), "an empty strip yields no targets and does not crash");
}

void TestAdvanceConvergesAndSnaps() {
  std::vector<float> current{0.0f};
  const std::vector<float> target{100.0f};
  bool moving = true;
  int steps = 0;
  while (moving && steps < 1000) {
    moving = AdvanceSlideOffsets(current, target, 16.0f);
    ++steps;
  }
  Expect(!moving, "the ease terminates rather than creeping forever");
  Expect(Near(current[0], 100.0f), "the offset lands exactly on its target after snapping");
  Expect(steps < 100, "convergence is brisk (well under a second at 60fps)");
}

void TestAdvanceSnapWithinThreshold() {
  std::vector<float> current{99.7f};
  const std::vector<float> target{100.0f};
  const bool moving = AdvanceSlideOffsets(current, target, 16.0f);
  Expect(!moving && current[0] == 100.0f,
         "an offset within the snap threshold jumps exactly to target and stops");
}

void TestAdvanceIsFrameRateIndependent() {
  // Exponential smoothing multiplies the remaining distance by exp(-dt/tau) each
  // step, so ten 16ms steps must land at the same place as one 160ms step.
  std::vector<float> many{0.0f};
  const std::vector<float> target{100.0f};
  for (int i = 0; i < 10; ++i) {
    AdvanceSlideOffsets(many, target, 16.0f);
  }
  std::vector<float> once{0.0f};
  AdvanceSlideOffsets(once, target, 160.0f);
  Expect(Near(many[0], once[0]),
         "the same total elapsed time converges to the same offset regardless of step count");
}

void TestSlideOffsetsMovingDetectsRest() {
  Expect(SlideOffsetsMoving({0.0f, 10.0f}, {0.0f, 0.0f}),
         "a non-trivial offset difference counts as moving");
  Expect(!SlideOffsetsMoving({0.0f, 0.0f}, {0.0f, 0.0f}),
         "matching offsets are at rest");
  Expect(!SlideOffsetsMoving({0.0f}, {0.0f, 0.0f}),
         "mismatched sizes are treated as not moving");
}

}  // namespace

void RegisterTabStripAnimationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TabStripAnimation/DragRightPullsNeighborLeftAndHoldsGap",
          TestSlideDragRightPullsNeighborLeftAndHoldsGap);
  AddTest(tests, "TabStripAnimation/HoverHomeMovesNothing", TestSlideHoverHomeMovesNothing);
  AddTest(tests, "TabStripAnimation/DropPastEndClosesUpBehind", TestSlideDropPastEndClosesUpBehind);
  AddTest(tests, "TabStripAnimation/DragLeftOpensGapAtFront", TestSlideDragLeftOpensGapAtFront);
  AddTest(tests, "TabStripAnimation/VariableWidths", TestSlideVariableWidths);
  AddTest(tests, "TabStripAnimation/EmptyStripIsSafe", TestSlideEmptyStripIsSafe);
  AddTest(tests, "TabStripAnimation/AdvanceConvergesAndSnaps", TestAdvanceConvergesAndSnaps);
  AddTest(tests, "TabStripAnimation/AdvanceSnapWithinThreshold", TestAdvanceSnapWithinThreshold);
  AddTest(tests, "TabStripAnimation/AdvanceIsFrameRateIndependent",
          TestAdvanceIsFrameRateIndependent);
  AddTest(tests, "TabStripAnimation/OffsetsMovingDetectsRest", TestSlideOffsetsMovingDetectsRest);
}

}  // namespace microide::tests
