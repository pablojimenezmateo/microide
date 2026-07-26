#include "TestSupport.h"

#include "app/BackgroundTaskCounter.h"
#include "util/SdlWake.h"

#include <SDL3/SDL.h>

namespace microide::tests {
namespace {

// SDL event subsystem is required for DecrementBackgroundTaskCountAndWake.
bool InitSdlEvents() {
  return SDL_Init(SDL_INIT_EVENTS);
}

void TestIncrementIncreasesCount() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  const int after = app::GetBackgroundTaskCount();
  app::DecrementBackgroundTaskCountAndWake();
  Expect(after == before + 1, "IncrementBackgroundTaskCount should increase count by 1");
}

void TestDecrementRestoresCount() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  app::DecrementBackgroundTaskCountAndWake();
  Expect(app::GetBackgroundTaskCount() == before,
         "matched increment+decrement should restore the original count");
}

void TestMultipleInFlightTasks() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  Expect(app::GetBackgroundTaskCount() == before + 3,
         "three increments should add 3 to the background task count");
  app::DecrementBackgroundTaskCountAndWake();
  app::DecrementBackgroundTaskCountAndWake();
  app::DecrementBackgroundTaskCountAndWake();
  Expect(app::GetBackgroundTaskCount() == before,
         "three balanced decrements should restore the original count");
}

// Verify count is always non-negative (underflow protection documented via SDL_assert).
void TestCountRemainsNonNegativeAfterBalancedOps() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  for (int i = 0; i < 5; ++i) {
    app::IncrementBackgroundTaskCount();
  }
  for (int i = 0; i < 5; ++i) {
    app::DecrementBackgroundTaskCountAndWake();
  }
  Expect(app::GetBackgroundTaskCount() >= 0,
         "background task count should never be negative");
  Expect(app::GetBackgroundTaskCount() == before,
         "count should return to baseline after balanced operations");
}

// Regression: the completion wake must post the dedicated registered event type,
// not the bare SDL_EVENT_USER base (which aliases the first registered custom
// event and mis-routes the wake into that subsystem's handler on every task).
void TestWakeUsesRegisteredEventTypeNotUserBase() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  Uint32 wake_type = SDL_RegisterEvents(1);
  // If this was the process's first registration it returns the SDL_EVENT_USER
  // base; take the next one so the wake type is provably distinct from the base
  // regardless of test ordering.
  if (wake_type == static_cast<Uint32>(SDL_EVENT_USER)) {
    wake_type = SDL_RegisterEvents(1);
  }
  Expect(wake_type != static_cast<Uint32>(-1), "SDL should allocate a wake event type");
  Expect(wake_type != static_cast<Uint32>(SDL_EVENT_USER),
         "the wake type used for this test must not equal the SDL_EVENT_USER base");
  app::SetBackgroundTaskWakeEventType(wake_type);

  // Drain any pending events so we observe only our wake.
  SDL_PumpEvents();
  SDL_Event drain{};
  while (SDL_PollEvent(&drain)) {
  }

  app::IncrementBackgroundTaskCount();
  app::DecrementBackgroundTaskCountAndWake();

  bool saw_wake = false;
  bool saw_user_base = false;
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    if (event.type == wake_type) saw_wake = true;
    if (event.type == static_cast<Uint32>(SDL_EVENT_USER)) saw_user_base = true;
  }
  Expect(saw_wake, "the completion wake must post the registered wake event type");
  Expect(!saw_user_base,
         "the completion wake must not post the aliasing SDL_EVENT_USER base");
}

// TD-2026-07-16-13: an unmatched decrement must saturate at zero, not go negative.
// A negative count would let the next real task increment -1 -> 0 and hide genuine
// in-flight work from the idle check. The saturating CAS makes this test safe to run
// (it no longer poisons the process-global counter for later tests).
void TestUnmatchedDecrementDoesNotGoNegative() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  // The process-global counter may be non-zero here in a full-suite run (a prior
  // test's async work). Drain it to the clamp floor first: the saturating decrement
  // can never push below zero, so over-decrementing settles at exactly zero. The
  // loop tolerates concurrent decrements from any still-draining prior work.
  const int baseline = app::GetBackgroundTaskCount();
  for (int i = 0; i < baseline + 4 && app::GetBackgroundTaskCount() > 0; ++i) {
    app::DecrementBackgroundTaskCountAndWake();
  }
  // Now decrement PAST zero: this is the underflow probe. Must stay clamped at zero.
  app::DecrementBackgroundTaskCountAndWake();
  Expect(app::GetBackgroundTaskCount() == 0,
         "an unmatched decrement must leave the count at zero, not negative");

  // The next legitimate task must be observable as in-flight (would read 0 if the
  // counter had gone to -1 above).
  app::IncrementBackgroundTaskCount();
  Expect(app::GetBackgroundTaskCount() == 1,
         "a real task after an underflow must still register as in-flight");
  app::DecrementBackgroundTaskCountAndWake();
  Expect(app::GetBackgroundTaskCount() == 0, "counter should return to zero");
}

// TD-2026-07-16-54: the completion wake routes through util::PushSdlWake, so a
// rejected push latches the shared "wake owed" bit (idle-poll fallback) instead of
// leaving the shell on a stale full-idle hint. Force a push failure via the seam.
void TestWakePushFailureLatchesOwedWake() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  microide::util::ConsumeOwedSdlWake();  // clear any prior owed state
  microide::util::SetSdlEventPusherForTesting([](const SDL_Event&) { return false; });

  app::IncrementBackgroundTaskCount();
  app::DecrementBackgroundTaskCountAndWake();  // its wake push will be rejected

  microide::util::SetSdlEventPusherForTesting(nullptr);  // restore
  Expect(microide::util::ConsumeOwedSdlWake(),
         "a rejected completion-wake push must latch the shared wake-owed bit");
}

}  // namespace

void RegisterBackgroundTaskCounterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BackgroundTaskCounter/WakePushFailureLatchesOwedWake",
          TestWakePushFailureLatchesOwedWake);
  AddTest(tests, "BackgroundTaskCounter/UnmatchedDecrementDoesNotGoNegative",
          TestUnmatchedDecrementDoesNotGoNegative);
  AddTest(tests, "BackgroundTaskCounter/WakeUsesRegisteredEventType",
          TestWakeUsesRegisteredEventTypeNotUserBase);
  AddTest(tests, "BackgroundTaskCounter/IncrementIncreasesCount",
          TestIncrementIncreasesCount);
  AddTest(tests, "BackgroundTaskCounter/DecrementRestoresCount",
          TestDecrementRestoresCount);
  AddTest(tests, "BackgroundTaskCounter/MultipleInFlightTasks",
          TestMultipleInFlightTasks);
  AddTest(tests, "BackgroundTaskCounter/CountRemainsNonNegative",
          TestCountRemainsNonNegativeAfterBalancedOps);
}

}  // namespace microide::tests
