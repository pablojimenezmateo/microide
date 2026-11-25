#include "TestSupport.h"

#include <cmath>
#include <vector>

#include "workspace/render/TabStripAnimation.h"

namespace microide::tests {
namespace {

using workspace::AdvanceSlideOffsets;
using workspace::ComputeSlideTargetXs;
using workspace::SlideOffsetsMoving;
using workspace::SlideTab;

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
  const std::vector<float> t = ComputeSlideTargetXs(tabs, 1, 3,
                                                    100.0f, 1.0f);
  Expect(t.size() == 4, "one target per input tab");
  Expect(Near(t[0], 0.0f), "tab 0 stays at the front");
  Expect(Near(t[2], 101.0f), "tab 2 slides left into the vacated slot 1");
  Expect(Near(t[3], 303.0f), "tab 3 keeps its slot; the gap opens in front of it");
}

void TestSlideHoverHomeMovesNothing() {
  const auto tabs = UniformStrip();
  const std::vector<float> t = ComputeSlideTargetXs(tabs, 1, 1,
                                                    100.0f, 1.0f);
  Expect(Near(t[0], 0.0f) && Near(t[2], 202.0f) && Near(t[3], 303.0f),
         "hovering the source slot leaves every neighbor at its base x");
}

void TestSlideDropPastEndClosesUpBehind() {
  const auto tabs = UniformStrip();
  const std::vector<float> t = ComputeSlideTargetXs(tabs, 1, 4,
                                                    100.0f, 1.0f);
  Expect(Near(t[2], 101.0f) && Near(t[3], 202.0f),
         "tabs 2 and 3 slide left to close the vacated slot; the gap trails the strip");
}

void TestSlideDragLeftOpensGapAtFront() {
  const auto tabs = UniformStrip();
  const std::vector<float> t = ComputeSlideTargetXs(tabs, 2, 0,
                                                    100.0f, 1.0f);
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
  const std::vector<float> t = ComputeSlideTargetXs(tabs, 0, 2,
                                                    132.0f, 1.0f);
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
  const bool changed = AdvanceSlideOffsets(current, target, 16.0f);
  Expect(changed && current[0] == 100.0f,
         "an offset within the snap threshold jumps exactly to target, and that is a change to paint");
  Expect(!AdvanceSlideOffsets(current, target, 16.0f), "an offset at its target has nothing left to change");
}

void TestAdvanceIsFrameRateIndependent() {
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

}

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

}
