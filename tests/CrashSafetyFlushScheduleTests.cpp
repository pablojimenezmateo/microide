#include "TestSupport.h"

#include "workspace/state/CrashSafetyFlushSchedule.h"

namespace microide::tests {
namespace {

using workspace::CrashSafetyFlushSchedule;
constexpr std::uint64_t kQuiet = CrashSafetyFlushSchedule::kQuietMs;
constexpr std::uint64_t kMax = CrashSafetyFlushSchedule::kMaxWaitMs;

void TestAQuietPauseFlushes() {
  CrashSafetyFlushSchedule schedule;
  Expect(!schedule.armed() && !schedule.DelayMs(0).has_value(), "nothing owed, nothing scheduled");
  schedule.Note(1000);
  Expect(schedule.DelayMs(1000) == kQuiet, "one edit: due a quiet window later");
  Expect(!schedule.Due(1000 + kQuiet - 1) && schedule.Due(1000 + kQuiet), "and not a tick before");
  schedule.Disarm();
  Expect(!schedule.Due(1000 + kQuiet), "flushed, it is owed nothing");
}

// The data-loss case: keys every second never leave a quiet window, and a pure
// debounce never flushed. The first unstaged edit bounds the wait.
void TestSteadyTypingStillFlushesWithinTheMaximumWait() {
  CrashSafetyFlushSchedule schedule;
  std::uint64_t now = 5000;
  schedule.Note(now);
  bool flushed = false;
  for (int key = 0; key < 600 && !flushed; ++key) {
    now += 1000;
    schedule.Note(now);
    flushed = schedule.Due(now);
  }
  Expect(flushed && now - 5000 <= kMax, "steady typing flushes by the maximum wait: " + std::to_string(now - 5000));
  schedule.Disarm();
  schedule.Note(now + 1);
  Expect(schedule.DelayMs(now + 1) == kQuiet, "the next edit starts a fresh window");
}

void TestATouchOnlyExtendsAnArmedSchedule() {
  CrashSafetyFlushSchedule schedule;
  schedule.Touch(100);
  Expect(!schedule.armed(), "an edit that leaves nothing dirty does not arm it");
  schedule.Note(100);
  schedule.Touch(2000);
  Expect(schedule.DelayMs(2000) == kQuiet, "but restarts the quiet window of one that is armed");
}

}  // namespace

void RegisterCrashSafetyFlushScheduleTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CrashSafetyFlushSchedule/AQuietPauseFlushes", TestAQuietPauseFlushes);
  AddTest(tests, "CrashSafetyFlushSchedule/SteadyTypingStillFlushesWithinTheMaximumWait",
          TestSteadyTypingStillFlushesWithinTheMaximumWait);
  AddTest(tests, "CrashSafetyFlushSchedule/ATouchOnlyExtendsAnArmedSchedule", TestATouchOnlyExtendsAnArmedSchedule);
}

}  // namespace microide::tests
