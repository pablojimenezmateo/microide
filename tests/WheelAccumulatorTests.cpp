#include "TestSupport.h"

#include <cmath>
#include <limits>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::tests {
namespace {

using workspace::AccumulateWheelEvent;
using workspace::InteractionState;
using workspace::WheelTicks;

void TestSubTickDeltasEmitNoScrollUntilThresholdCrossed() {
  // Smooth-scroll trackpad: each event reports y ~ 0.3. Three events should
  // accumulate to exactly one whole-line tick. The legacy integer_y path
  // dropped each of these to zero and emitted no scroll at all until a
  // discrete tick eventually landed.
  InteractionState state;
  WheelTicks ticks = AccumulateWheelEvent(state, 0.3f, 0.0f, false);
  Expect(ticks.vertical == 0,
         "first sub-tick should not emit a scroll line");
  ticks = AccumulateWheelEvent(state, 0.3f, 0.0f, false);
  Expect(ticks.vertical == 0,
         "second sub-tick should not emit a scroll line");
  ticks = AccumulateWheelEvent(state, 0.5f, 0.0f, false);
  Expect(ticks.vertical == 1,
         "third sub-tick should emit exactly one scroll line once |accum| crosses 1");
  Expect(std::fabs(state.wheel_accumulator_y - 0.1f) < 1e-5f,
         "residual fractional accumulation must survive across events");
}

void TestDiscreteWheelClickEmitsOneTickImmediately() {
  InteractionState state;
  const WheelTicks ticks = AccumulateWheelEvent(state, 1.0f, 0.0f, false);
  Expect(ticks.vertical == 1,
         "a y=1.0 discrete wheel click must emit a single line of scroll");
  Expect(std::fabs(state.wheel_accumulator_y) < 1e-5f,
         "discrete click should leave no residual");
}

void TestDirectionReversalDropsOppositeResidue() {
  // The user scrolled down enough to leave +0.7 of residue, then reverses.
  // We should not require the reverse to "burn through" the residue before
  // emitting a scroll-up tick.
  InteractionState state;
  AccumulateWheelEvent(state, 0.7f, 0.0f, false);  // +0.7 accumulated, no tick
  Expect(state.wheel_accumulator_y > 0.0f,
         "down-scroll residue is positive after initial event");
  const WheelTicks ticks = AccumulateWheelEvent(state, -1.0f, 0.0f, false);
  Expect(ticks.vertical == -1,
         "reversing direction must emit immediately; residue from the other "
         "direction is discarded");
}

void TestFlippedDirectionInvertsSign() {
  // SDL_MOUSEWHEEL_FLIPPED means the OS has already inverted natural-scrolling.
  // We must apply that flip exactly once.
  InteractionState state;
  const WheelTicks normal = AccumulateWheelEvent(state, 1.0f, 0.0f, false);
  Expect(normal.vertical == 1, "non-flipped y=1 emits +1 tick");

  InteractionState flipped_state;
  const WheelTicks flipped = AccumulateWheelEvent(flipped_state, 1.0f, 0.0f, true);
  Expect(flipped.vertical == -1,
         "flipped y=1 must emit -1 tick (natural-scrolling inverted)");
}

void TestHorizontalAxisAccumulatesIndependently() {
  InteractionState state;
  AccumulateWheelEvent(state, 0.0f, 0.4f, false);
  AccumulateWheelEvent(state, 0.0f, 0.4f, false);
  const WheelTicks ticks = AccumulateWheelEvent(state, 0.0f, 0.4f, false);
  Expect(ticks.horizontal == 1,
         "horizontal accumulator should emit one tick after crossing 1");
  Expect(ticks.vertical == 0,
         "vertical accumulator must not be touched by horizontal events");
}

void TestNonFiniteDeltasAreIgnored() {
  // A stray NaN or infinity in the SDL event should not poison the
  // accumulator (the rest of the session would silently break otherwise).
  InteractionState state;
  state.wheel_accumulator_y = 0.5f;
  AccumulateWheelEvent(state, std::numeric_limits<float>::quiet_NaN(), 0.0f, false);
  Expect(std::fabs(state.wheel_accumulator_y - 0.5f) < 1e-5f,
         "NaN delta must leave the accumulator unchanged");
  AccumulateWheelEvent(state, std::numeric_limits<float>::infinity(), 0.0f, false);
  Expect(std::fabs(state.wheel_accumulator_y - 0.5f) < 1e-5f,
         "infinite delta must leave the accumulator unchanged");
}

void TestLargeDeltasEmitMultipleTicks() {
  // A high-resolution device that reports a large delta in one event
  // (e.g. y=3.5) should emit floor(3.5) = 3 ticks plus carry the 0.5
  // residue forward.
  InteractionState state;
  const WheelTicks ticks = AccumulateWheelEvent(state, 3.5f, 0.0f, false);
  Expect(ticks.vertical == 3,
         "y=3.5 should emit exactly 3 ticks (the int part)");
  Expect(std::fabs(state.wheel_accumulator_y - 0.5f) < 1e-5f,
         "fractional 0.5 must remain as residue for the next event");
}

}  // namespace

void RegisterWheelAccumulatorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WheelAccumulator/SubTickDeltasEmitNoScrollUntilThreshold",
          TestSubTickDeltasEmitNoScrollUntilThresholdCrossed);
  AddTest(tests, "WheelAccumulator/DiscreteWheelClickEmitsOneTickImmediately",
          TestDiscreteWheelClickEmitsOneTickImmediately);
  AddTest(tests, "WheelAccumulator/DirectionReversalDropsOppositeResidue",
          TestDirectionReversalDropsOppositeResidue);
  AddTest(tests, "WheelAccumulator/FlippedDirectionInvertsSign",
          TestFlippedDirectionInvertsSign);
  AddTest(tests, "WheelAccumulator/HorizontalAxisAccumulatesIndependently",
          TestHorizontalAxisAccumulatesIndependently);
  AddTest(tests, "WheelAccumulator/NonFiniteDeltasAreIgnored",
          TestNonFiniteDeltasAreIgnored);
  AddTest(tests, "WheelAccumulator/LargeDeltasEmitMultipleTicks",
          TestLargeDeltasEmitMultipleTicks);
}

}  // namespace microide::tests
