// util::MainThreadMailbox tests.
//
// Cover the outbound mailbox contract that the plugin worker and the DAP/LSP
// clients all build on: actions queue and drain in FIFO order, the lockless
// pending gate stays accurate, Clear drops without running, and the SDL wake
// event fires exactly once per Post / once per explicit PushWake (and never
// when waking is disabled).

#include "TestSupport.h"

#include "util/MainThreadMailbox.h"
#include "util/SdlWake.h"

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

// TD-2026-07-17A-018: PostLatest coalesces replaceable actions by key so a chatty
// producer cannot stack many stale closures between drains. Same-key posts replace
// in place (only the newest runs, in the first occurrence's position); distinct
// keys and plain posts are independent.
void TestPostLatestCoalescesByKey() {
  MainThreadMailbox mailbox;
  mailbox.SetWakeEventType(0);  // wake disabled: no SDL needed
  std::vector<int> ran;

  // Three posts under the same key: only the last survives, keeping one slot.
  mailbox.PostLatest("k", [&ran]() { ran.push_back(1); });
  mailbox.PostLatest("k", [&ran]() { ran.push_back(2); });
  mailbox.PostLatest("k", [&ran]() { ran.push_back(3); });
  Expect(mailbox.PendingCount() == 1, "same-key PostLatest coalesces to one queued action");

  // A distinct key and a plain post add their own slots.
  mailbox.PostLatest("other", [&ran]() { ran.push_back(10); });
  mailbox.PostWithoutWake([&ran]() { ran.push_back(20); });
  Expect(mailbox.PendingCount() == 3, "distinct keys and plain posts are independent slots");

  Expect(mailbox.Drain() == 3, "drain runs the three retained actions");
  Expect(ran.size() == 3 && ran[0] == 3 && ran[1] == 10 && ran[2] == 20,
         "only the newest same-key action runs, in the original slot order");

  // After a drain the key index resets: a later same-key post queues fresh.
  ran.clear();
  mailbox.PostLatest("k", [&ran]() { ran.push_back(99); });
  Expect(mailbox.PendingCount() == 1, "the key index resets after Drain");
  mailbox.Drain();
  Expect(ran.size() == 1 && ran[0] == 99, "the post-drain same-key action runs");
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

// TD-2026-07-16-53: when SDL_PushEvent rejects the wake, the queued action must stay
// pending AND the mailbox must latch an "undelivered wake" so the scheduled poll can
// retry — silently losing the only wake would strand the action.
void TestPushWakeFailureLeavesActionPending() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  const Uint32 type = SDL_RegisterEvents(1);
  Expect(type != 0 && type != static_cast<Uint32>(-1), "registered an SDL event type");

  MainThreadMailbox::SetEventPusherForTesting([](const SDL_Event&) { return false; });
  MainThreadMailbox mailbox;
  mailbox.SetWakeEventType(type);

  bool ran = false;
  mailbox.Post([&ran]() { ran = true; });
  Expect(mailbox.PendingCount() == 1, "a failed wake push must not drop the queued action");
  Expect(mailbox.HasUndeliveredWake(),
         "a failed wake push while work is pending latches an undelivered-wake bit");
  Expect(!ran, "the action has not run yet (no drain)");

  MainThreadMailbox::SetEventPusherForTesting(nullptr);  // restore before draining
  Expect(mailbox.Drain() == 1 && ran, "the still-pending action drains and runs");
  Expect(!mailbox.HasUndeliveredWake(), "draining clears the undelivered-wake bit");
}

// The undelivered wake retries successfully once the event queue accepts pushes again.
void TestPostWakeFailureCanRetry() {
  static const bool initialized = InitSdlEvents();
  (void)initialized;
  const Uint32 type = SDL_RegisterEvents(1);
  Expect(type != 0 && type != static_cast<Uint32>(-1), "registered an SDL event type");

  MainThreadMailbox::SetEventPusherForTesting([](const SDL_Event&) { return false; });
  MainThreadMailbox mailbox;
  mailbox.SetWakeEventType(type);
  mailbox.Post([]() {});
  Expect(mailbox.HasUndeliveredWake(), "wake push failed, bit latched");
  Expect(mailbox.RetryWakeIfPending(), "retry still owes a wake while the queue rejects");

  // Queue recovers: the retry now delivers and clears the bit.
  FlushEvents(type);
  MainThreadMailbox::SetEventPusherForTesting(nullptr);
  Expect(!mailbox.RetryWakeIfPending(), "a successful retry clears the owed wake");
  Expect(!mailbox.HasUndeliveredWake(), "no wake owed after a successful retry");
  Expect(CountEvents(type) == 1, "the retry delivered exactly one wake event");
  mailbox.Drain();
}

// TD-2026-07-16-56: the wake-registration-degraded flag drives the idle-wait fallback
// poll. Verify it round-trips (the idle-wait consumer clamps its timeout when set).
void TestSdlWakeRegistrationDegradedFlagRoundTrips() {
  using microide::util::SdlWakeRegistrationDegraded;
  using microide::util::SetSdlWakeRegistrationDegraded;
  const bool prior = SdlWakeRegistrationDegraded();
  SetSdlWakeRegistrationDegraded(true);
  Expect(SdlWakeRegistrationDegraded(), "degraded flag reads true after being set");
  SetSdlWakeRegistrationDegraded(false);
  Expect(!SdlWakeRegistrationDegraded(), "degraded flag clears");
  SetSdlWakeRegistrationDegraded(prior);  // restore
}

}  // namespace

void RegisterMainThreadMailboxTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MainThreadMailbox/SdlWakeRegistrationDegradedFlagRoundTrips",
          TestSdlWakeRegistrationDegradedFlagRoundTrips);
  AddTest(tests, "MainThreadMailbox/PushWakeFailureLeavesActionPending",
          TestPushWakeFailureLeavesActionPending);
  AddTest(tests, "MainThreadMailbox/PostWakeFailureCanRetry", TestPostWakeFailureCanRetry);
  AddTest(tests, "MainThreadMailbox/DrainRunsActionsInOrder", TestDrainRunsActionsInOrder);
  AddTest(tests, "MainThreadMailbox/PostLatestCoalescesByKey", TestPostLatestCoalescesByKey);
  AddTest(tests, "MainThreadMailbox/ClearDropsWithoutRunning", TestClearDropsWithoutRunning);
  AddTest(tests, "MainThreadMailbox/PostPushesExactlyOneWakeEvent",
          TestPostPushesExactlyOneWakeEvent);
  AddTest(tests, "MainThreadMailbox/BatchPostWakesOnce", TestBatchPostWakesOnce);
  AddTest(tests, "MainThreadMailbox/WakeDisabledPushesNoEvent", TestWakeDisabledPushesNoEvent);
}

}  // namespace microide::tests
