#include "TestSupport.h"

#include "app/BackgroundTaskCounter.h"

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

// Verify the IdleHint::Full condition: count > 0 represents in-flight work.
void TestIdleHintFullConditionHoldsWhenCountPositive() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  // ComputeIdleHint(GetBackgroundTaskCount()) would return IdleHint::Full here
  // because in_flight_background_task_count > 0.
  const bool would_be_full = app::GetBackgroundTaskCount() > 0;
  app::DecrementBackgroundTaskCountAndWake();
  Expect(would_be_full, "count > 0 should satisfy the IdleHint::Full condition");
  (void)before;
}

// Verify the IdleHint::Idle condition: count == 0 when no tasks in flight.
void TestIdleHintIdleConditionHoldsWhenCountZero() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = app::GetBackgroundTaskCount();
  app::IncrementBackgroundTaskCount();
  app::DecrementBackgroundTaskCountAndWake();
  // With count restored to `before`, if before == 0 then IdleHint::Idle applies
  // (assuming no plugin async processes and no caret blink).
  const bool idle_condition_met = app::GetBackgroundTaskCount() <= 0;
  Expect(idle_condition_met || before > 0,
         "after balanced operations, count should satisfy IdleHint::Idle precondition");
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

}  // namespace

void RegisterBackgroundTaskCounterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BackgroundTaskCounter/WakeUsesRegisteredEventType",
          TestWakeUsesRegisteredEventTypeNotUserBase);
  AddTest(tests, "BackgroundTaskCounter/IncrementIncreasesCount",
          TestIncrementIncreasesCount);
  AddTest(tests, "BackgroundTaskCounter/DecrementRestoresCount",
          TestDecrementRestoresCount);
  AddTest(tests, "BackgroundTaskCounter/MultipleInFlightTasks",
          TestMultipleInFlightTasks);
  AddTest(tests, "BackgroundTaskCounter/IdleHintFullCondition",
          TestIdleHintFullConditionHoldsWhenCountPositive);
  AddTest(tests, "BackgroundTaskCounter/IdleHintIdleCondition",
          TestIdleHintIdleConditionHoldsWhenCountZero);
  AddTest(tests, "BackgroundTaskCounter/CountRemainsNonNegative",
          TestCountRemainsNonNegativeAfterBalancedOps);
}

}  // namespace microide::tests
