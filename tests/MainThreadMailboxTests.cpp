// util::MainThreadMailbox tests.
//
// Cover the outbound mailbox contract that the plugin worker and the DAP/LSP
// clients all build on: actions queue and drain in FIFO order, the lockless
// pending gate stays accurate, Clear drops without running, and the SDL wake
// event fires exactly once per Post / once per explicit PushWake (and never
// when waking is disabled).

#include "TestSupport.h"

#include "util/MainThreadMailbox.h"

#include <SDL3/SDL.h>

#include <vector>

namespace microide::tests {
namespace {

using microide::util::MainThreadMailbox;

bool InitSdlEvents() { return SDL_Init(SDL_INIT_EVENTS); }

// Remove any pending events of `type` from the queue so a subsequent count is
// isolated to events pushed after this point.
void FlushEvents(Uint32 type) {
  SDL_PumpEvents();
  SDL_Event scratch[64];
  while (SDL_PeepEvents(scratch, 64, SDL_GETEVENT, type, type) > 0) {
    // drain
  }
}

int CountEvents(Uint32 type) {
  SDL_PumpEvents();
  SDL_Event scratch[64];
  return SDL_PeepEvents(scratch, 64, SDL_GETEVENT, type, type);
}

void TestDrainRunsActionsInOrder() {
  MainThreadMailbox mailbox;
  std::vector<int> order;
  mailbox.PostWithoutWake([&order]() { order.push_back(1); });
  mailbox.PostWithoutWake([&order]() { order.push_back(2); });
  mailbox.PostWithoutWake([&order]() { order.push_back(3); });

  Expect(mailbox.PendingCount() == 3, "PendingCount reflects queued actions");
  const int drained = mailbox.Drain();
  Expect(drained == 3, "Drain runs and counts every queued action");
  Expect(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3,
         "Drain runs actions in FIFO order");
  Expect(mailbox.PendingCount() == 0, "PendingCount resets to 0 after Drain");
  Expect(mailbox.Drain() == 0, "draining an empty mailbox is a no-op");
}

void TestClearDropsWithoutRunning() {
  MainThreadMailbox mailbox;
  bool ran = false;
  mailbox.PostWithoutWake([&ran]() { ran = true; });
  Expect(mailbox.PendingCount() == 1, "action queued before Clear");
  mailbox.Clear();
  Expect(mailbox.PendingCount() == 0, "Clear empties the mailbox");
  Expect(mailbox.Drain() == 0, "no action survives Clear");
  Expect(!ran, "Clear must not run the dropped action");
}

void TestPostPushesExactlyOneWakeEvent() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  const Uint32 type = SDL_RegisterEvents(1);
  Expect(type != 0 && type != static_cast<Uint32>(-1), "registered an SDL event type");

  MainThreadMailbox mailbox;
  mailbox.SetWakeEventType(type);
  FlushEvents(type);

  bool ran = false;
  mailbox.Post([&ran]() { ran = true; });
  Expect(CountEvents(type) == 1, "Post pushes exactly one wake event");
  Expect(mailbox.Drain() == 1 && ran, "the posted action drains and runs");
}

void TestBatchPostWakesOnce() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  const Uint32 type = SDL_RegisterEvents(1);
  Expect(type != 0 && type != static_cast<Uint32>(-1), "registered an SDL event type");

  MainThreadMailbox mailbox;
  mailbox.SetWakeEventType(type);
  FlushEvents(type);

  for (int i = 0; i < 5; ++i) {
    mailbox.PostWithoutWake([]() {});
  }
  Expect(CountEvents(type) == 0, "PostWithoutWake pushes no wake events");
  mailbox.PushWake();
  Expect(CountEvents(type) == 1, "a batch of PostWithoutWake + one PushWake wakes once");
  Expect(mailbox.Drain() == 5, "all batched actions drain");
}

void TestWakeDisabledPushesNoEvent() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  const Uint32 type = SDL_RegisterEvents(1);
  Expect(type != 0 && type != static_cast<Uint32>(-1), "registered an SDL event type");

  MainThreadMailbox mailbox;
  // Wake event type left at 0 (disabled).
  FlushEvents(type);
  mailbox.Post([]() {});
  mailbox.PushWake();
  Expect(CountEvents(type) == 0, "no wake event fires when the wake type is 0");
  Expect(mailbox.Drain() == 1, "the action still queues and drains");
}

}  // namespace

void RegisterMainThreadMailboxTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MainThreadMailbox/DrainRunsActionsInOrder", TestDrainRunsActionsInOrder);
  AddTest(tests, "MainThreadMailbox/ClearDropsWithoutRunning", TestClearDropsWithoutRunning);
  AddTest(tests, "MainThreadMailbox/PostPushesExactlyOneWakeEvent",
          TestPostPushesExactlyOneWakeEvent);
  AddTest(tests, "MainThreadMailbox/BatchPostWakesOnce", TestBatchPostWakesOnce);
  AddTest(tests, "MainThreadMailbox/WakeDisabledPushesNoEvent", TestWakeDisabledPushesNoEvent);
}

}  // namespace microide::tests
