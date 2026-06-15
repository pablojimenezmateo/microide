#include "TestSupport.h"

#include "app/ApplicationPresentationCache.h"
#include "app/IdleWaitStrategy.h"
#include "app/RedrawTraceAccumulator.h"

namespace microide::tests {
namespace {

using microide::app::CanReuseCachedPresentationState;
using microide::app::ChooseIdleWait;
using microide::app::IdleWaitMode;
using microide::app::RedrawFrameStats;
using microide::app::RedrawTraceAccumulator;
using WindowPresentationState = microide::workspace::WorkspaceShell::WindowPresentationState;
using IdleWaitState = microide::workspace::WorkspaceShell::IdleWaitState;
using IdleHint = microide::workspace::WorkspaceShell::IdleHint;

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

void TestChooseIdleWaitMapsHints() {
  const auto full = ChooseIdleWait(IdleWaitState{.hint = IdleHint::Full});
  Expect(full.mode == IdleWaitMode::Poll, "Full hint should poll without blocking");

  const auto idle = ChooseIdleWait(IdleWaitState{.hint = IdleHint::Idle});
  Expect(idle.mode == IdleWaitMode::Wait, "Idle hint should block on SDL_WaitEvent");

  const auto caret =
      ChooseIdleWait(IdleWaitState{.hint = IdleHint::CaretOnly, .caret_remaining_ms = 250});
  Expect(caret.mode == IdleWaitMode::WaitTimeout, "CaretOnly hint should wait with a timeout");
  Expect(caret.timeout_ms == 250, "CaretOnly timeout should pass through the remaining ms");

  const auto caret_zero =
      ChooseIdleWait(IdleWaitState{.hint = IdleHint::CaretOnly, .caret_remaining_ms = 0});
  Expect(caret_zero.timeout_ms == 1,
         "CaretOnly should clamp a zero timeout to 1ms so the loop never busy-spins");
}

void TestRedrawTraceAccumulatorCountsAndFlushes() {
  RedrawTraceAccumulator accumulator;
  accumulator.Configure(/*enabled=*/true, /*verbose=*/false);

  const RedrawFrameStats full_frame{
      .full_redraw = true, .dirty_rect_count = 0, .rendered_clip_count = 0, .reason = "full"};
  const RedrawFrameStats partial_frame{
      .full_redraw = false, .dirty_rect_count = 3, .rendered_clip_count = 2, .reason = "partial"};

  accumulator.Record(full_frame);
  accumulator.Record(partial_frame);
  accumulator.Record(partial_frame);
  Expect(accumulator.frames() == 3, "every recorded frame should be counted");
  Expect(accumulator.full_frames() == 1, "full frames should be tallied separately");
  Expect(accumulator.partial_frames() == 2, "partial frames should be tallied separately");

  // Reaching the log interval flushes and resets the rolling counters.
  for (Uint64 i = accumulator.frames(); i < RedrawTraceAccumulator::kLogInterval; ++i) {
    accumulator.Record(full_frame);
  }
  Expect(accumulator.frames() == 0, "the accumulator should reset after flushing at the interval");
}

void TestRedrawTraceAccumulatorIgnoresFramesWhenDisabled() {
  RedrawTraceAccumulator accumulator;  // disabled by default
  accumulator.Record(RedrawFrameStats{.full_redraw = true, .reason = "full"});
  Expect(accumulator.frames() == 0,
         "a disabled accumulator should not tally frames");
}

}  // namespace

void RegisterApplicationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Application/PresentationCacheReusedWhenUiScaleMatches",
          TestPresentationCacheReusedWhenUiScaleMatches);
  AddTest(tests, "Application/PresentationCacheInvalidatedWhenUiScaleChanges",
          TestPresentationCacheInvalidatedWhenUiScaleChanges);
  AddTest(tests, "Application/ChooseIdleWaitMapsHints", TestChooseIdleWaitMapsHints);
  AddTest(tests, "Application/RedrawTraceAccumulatorCountsAndFlushes",
          TestRedrawTraceAccumulatorCountsAndFlushes);
  AddTest(tests, "Application/RedrawTraceAccumulatorIgnoresFramesWhenDisabled",
          TestRedrawTraceAccumulatorIgnoresFramesWhenDisabled);
}

}  // namespace microide::tests
