#include "TestSupport.h"

#include "util/BackgroundTaskCounter.h"
#include "util/Waker.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <thread>

namespace microide::tests {
namespace {

bool InitSdlEvents() {
  return SDL_Init(SDL_INIT_EVENTS);
}

void TestIncrementIncreasesCount() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = util::GetBackgroundTaskCount();
  util::IncrementBackgroundTaskCount();
  const int after = util::GetBackgroundTaskCount();
  util::DecrementBackgroundTaskCountAndWake();
  Expect(after == before + 1, "IncrementBackgroundTaskCount should increase count by 1");
}

void TestDecrementRestoresCount() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = util::GetBackgroundTaskCount();
  util::IncrementBackgroundTaskCount();
  util::DecrementBackgroundTaskCountAndWake();
  Expect(util::GetBackgroundTaskCount() == before,
         "matched increment+decrement should restore the original count");
}

void TestMultipleInFlightTasks() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = util::GetBackgroundTaskCount();
  util::IncrementBackgroundTaskCount();
  util::IncrementBackgroundTaskCount();
  util::IncrementBackgroundTaskCount();
  Expect(util::GetBackgroundTaskCount() == before + 3,
         "three increments should add 3 to the background task count");
  util::DecrementBackgroundTaskCountAndWake();
  util::DecrementBackgroundTaskCountAndWake();
  util::DecrementBackgroundTaskCountAndWake();
  Expect(util::GetBackgroundTaskCount() == before,
         "three balanced decrements should restore the original count");
}

void TestCountRemainsNonNegativeAfterBalancedOps() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int before = util::GetBackgroundTaskCount();
  for (int i = 0; i < 5; ++i) {
    util::IncrementBackgroundTaskCount();
  }
  for (int i = 0; i < 5; ++i) {
    util::DecrementBackgroundTaskCountAndWake();
  }
  Expect(util::GetBackgroundTaskCount() >= 0,
         "background task count should never be negative");
  Expect(util::GetBackgroundTaskCount() == before,
         "count should return to baseline after balanced operations");
}

void TestWakeUsesRegisteredEventTypeNotUserBase() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  Uint32 wake_type = SDL_RegisterEvents(1);
  if (wake_type == static_cast<Uint32>(SDL_EVENT_USER)) {
    wake_type = SDL_RegisterEvents(1);
  }
  Expect(wake_type != static_cast<Uint32>(-1), "SDL should allocate a wake event type");
  Expect(wake_type != static_cast<Uint32>(SDL_EVENT_USER),
         "the wake type used for this test must not equal the SDL_EVENT_USER base");
  util::SetBackgroundTaskWakeChannel(wake_type);

  SDL_PumpEvents();
  SDL_Event drain{};
  while (SDL_PollEvent(&drain)) {
  }

  util::IncrementBackgroundTaskCount();
  util::DecrementBackgroundTaskCountAndWake();

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

void TestUnmatchedDecrementDoesNotGoNegative() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;

  const int baseline = util::GetBackgroundTaskCount();
  for (int i = 0; i < baseline + 4 && util::GetBackgroundTaskCount() > 0; ++i) {
    util::DecrementBackgroundTaskCountAndWake();
  }
  util::DecrementBackgroundTaskCountAndWake();
  Expect(util::GetBackgroundTaskCount() == 0,
         "an unmatched decrement must leave the count at zero, not negative");

  util::IncrementBackgroundTaskCount();
  Expect(util::GetBackgroundTaskCount() == 1,
         "a real task after an underflow must still register as in-flight");
  util::DecrementBackgroundTaskCountAndWake();
  Expect(util::GetBackgroundTaskCount() == 0, "counter should return to zero");
}

void TestWakePushFailureLatchesOwedWake() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  util::SetBackgroundTaskWakeChannel(4243);
  microide::util::ConsumeOwedWake();
  microide::util::SetWakePusherForTesting([](microide::util::WakeChannel) { return false; });

  util::IncrementBackgroundTaskCount();
  util::DecrementBackgroundTaskCountAndWake();

  microide::util::SetWakePusherForTesting(nullptr);
  Expect(microide::util::ConsumeOwedWake(),
         "a rejected completion-wake push must latch the shared wake-owed bit");
}

void TestWakePusherOverrideSwapsSafelyUnderConcurrentWakes() {
  std::atomic<bool> stop{false};
  std::thread pusher([&stop] {
    while (!stop.load(std::memory_order_acquire)) {
      (void)microide::util::DeliverWake(4244);
    }
  });
  bool reached_after_clear = false;
  for (int round = 0; round < 200; ++round) {
    std::atomic<int> calls{0};
    std::atomic<bool> cleared{false};
    microide::util::SetWakePusherForTesting([&calls, &cleared, &reached_after_clear](microide::util::WakeChannel) {
      reached_after_clear = reached_after_clear || cleared.load(std::memory_order_acquire);
      calls.fetch_add(1, std::memory_order_relaxed);
      return true;
    });
    while (calls.load(std::memory_order_relaxed) == 0) {
      std::this_thread::yield();
    }
    microide::util::SetWakePusherForTesting(nullptr);
    cleared.store(true, std::memory_order_release);
  }
  stop.store(true, std::memory_order_release);
  pusher.join();
  Expect(!reached_after_clear, "no call reaches an override after it was cleared");
}

}

void RegisterBackgroundTaskCounterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BackgroundTaskCounter/WakePushFailureLatchesOwedWake",
          TestWakePushFailureLatchesOwedWake);
  AddTest(tests, "BackgroundTaskCounter/WakePusherOverrideSwapsSafelyUnderConcurrentWakes",
          TestWakePusherOverrideSwapsSafelyUnderConcurrentWakes);
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

}
