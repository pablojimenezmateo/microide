// The guarded-completion primitive every off-thread buffer operation shares.
//
// The rule it encodes is two-part and both halves were hand-rolled before it
// existed: a completion must find the tab that posted it (and find none if that
// tab is gone), and it must refuse to apply its result to a buffer that moved on
// while the work ran. The cases worth pinning are the ones where a hand-rolled
// version silently does the wrong thing — a duplicate delivery, a re-arm over an
// in-flight run, and the id-zero disarmed state.

#include "TestSupport.h"

#include <cstdint>
#include <vector>

#include "editor/AsyncBufferWork.h"

namespace microide::tests {
namespace {

using microide::editor::AsyncBufferWork;
using Claim = microide::editor::AsyncBufferWork::Claim;

void TestAnUntouchedBufferClaimsCurrent() {
  AsyncBufferWork work;
  Expect(!work.armed(), "a fresh slot is disarmed");
  const std::uint64_t id = work.Post(/*content_revision=*/7);
  Expect(id != 0, "a posted operation has a non-zero id");
  Expect(work.armed() && work.Holds(id), "the slot holds the id it minted");
  Expect(work.Resolve(id, 7) == Claim::Current, "an unchanged buffer applies the result");
  Expect(!work.armed(), "resolving consumes the slot");
}

void TestAnEditedBufferClaimsStale() {
  AsyncBufferWork work;
  const std::uint64_t id = work.Post(7);
  Expect(work.Resolve(id, 8) == Claim::Stale, "a buffer edited under the work drops the result");
  Expect(!work.armed(), "a stale claim consumes the slot too");
}

// The walk that finds a completion's tab asks every tab, so every OTHER tab must
// answer NotMine without consuming its own in-flight operation.
void TestAnotherTabsCompletionLeavesThisSlotAlone() {
  AsyncBufferWork mine;
  const std::uint64_t my_id = mine.Post(3);
  const std::uint64_t other_id = AsyncBufferWork::NextId();
  Expect(other_id != my_id, "ids are not reused across operations");
  Expect(!mine.Holds(other_id), "a foreign id does not match");
  Expect(mine.Resolve(other_id, 3) == Claim::NotMine, "a foreign completion claims nothing");
  Expect(mine.armed() && mine.Holds(my_id), "and leaves this slot's own run in flight");
  Expect(mine.Resolve(my_id, 3) == Claim::Current, "which still resolves normally");
}

// A completion delivered twice must apply once. The slot is consumed by the first
// claim, so the second reads NotMine rather than reloading the buffer again.
void TestADuplicateCompletionAppliesOnce() {
  AsyncBufferWork work;
  const std::uint64_t id = work.Post(1);
  Expect(work.Resolve(id, 1) == Claim::Current, "the first delivery applies");
  Expect(work.Resolve(id, 1) == Claim::NotMine, "the second does not");
}

// Re-arming is what a second Ctrl+S, or a tab retargeted at another file, does.
// The superseded run's completion must not apply to the buffer the NEW run is
// about, even when the revision happens to match.
void TestRearmingAbandonsTheEarlierRun() {
  AsyncBufferWork work;
  const std::uint64_t first = work.Post(5);
  const std::uint64_t second = work.Post(5);
  Expect(first != second, "the second post mints a new id");
  Expect(work.Resolve(first, 5) == Claim::NotMine,
         "the superseded run claims nothing even at a matching revision");
  Expect(work.Holds(second), "and the live run is untouched");
  Expect(work.Resolve(second, 5) == Claim::Current, "so the live run still applies");
}

// Zero is the disarmed state, not an id. A caller that posts nothing (an empty
// formatter command returns id 0) must not have its next completion swallowed by
// a slot that thinks it is waiting.
void TestZeroIsNeverAMatch() {
  AsyncBufferWork work;
  Expect(!work.Holds(0), "a disarmed slot does not hold id 0");
  Expect(work.Resolve(0, 0) == Claim::NotMine, "nor resolves it");
  work.Arm(9, 4);
  Expect(!work.Holds(0), "an armed slot does not hold id 0 either");
  work.Disarm();
  Expect(!work.armed() && work.Resolve(9, 4) == Claim::NotMine,
         "an explicitly disarmed slot drops its run's completion");
}

}  // namespace

void RegisterAsyncBufferWorkTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AsyncBufferWork/AnUntouchedBufferClaimsCurrent", TestAnUntouchedBufferClaimsCurrent);
  AddTest(tests, "AsyncBufferWork/AnEditedBufferClaimsStale", TestAnEditedBufferClaimsStale);
  AddTest(tests, "AsyncBufferWork/AnotherTabsCompletionLeavesThisSlotAlone",
          TestAnotherTabsCompletionLeavesThisSlotAlone);
  AddTest(tests, "AsyncBufferWork/ADuplicateCompletionAppliesOnce", TestADuplicateCompletionAppliesOnce);
  AddTest(tests, "AsyncBufferWork/RearmingAbandonsTheEarlierRun", TestRearmingAbandonsTheEarlierRun);
  AddTest(tests, "AsyncBufferWork/ZeroIsNeverAMatch", TestZeroIsNeverAMatch);
}

}  // namespace microide::tests
