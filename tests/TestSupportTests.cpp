#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace microide::tests {
namespace {

using namespace std::chrono_literals;

// A predicate that flips to true shortly after WaitUntil starts should be
// observed well before the timeout — WaitUntil returns as soon as it holds, it
// does not sleep out the full timeout.
void TestWaitUntilReturnsPromptlyWhenPredicateFlips() {
  std::atomic<bool> ready{false};
  std::thread flip([&ready]() {
    std::this_thread::sleep_for(20ms);
    ready.store(true);
  });

  const auto start = std::chrono::steady_clock::now();
  const bool ok = WaitUntil([&ready]() { return ready.load(); },
                            /*timeout=*/2s, /*poll_interval=*/2ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  flip.join();

  Expect(ok, "WaitUntil should observe the predicate becoming true");
  Expect(elapsed < 1s,
         "WaitUntil should return soon after the predicate flips, not sleep out the timeout");
}

// A predicate that never holds returns false after roughly the timeout.
void TestWaitUntilReturnsFalseOnTimeout() {
  const auto start = std::chrono::steady_clock::now();
  const bool ok = WaitUntil([]() { return false; },
                            /*timeout=*/60ms, /*poll_interval=*/5ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  Expect(!ok, "WaitUntil should return false when the predicate never holds");
  Expect(elapsed >= 60ms, "WaitUntil should wait at least the full timeout before giving up");
}

// `pump` runs before every predicate check (and once more after the deadline),
// so a mailbox-style drain advances the awaited state between checks.
void TestWaitUntilRunsPumpEachIteration() {
  int pump_count = 0;
  int checks_needed = 3;
  const bool ok = WaitUntil(
      [&pump_count, checks_needed]() { return pump_count >= checks_needed; },
      /*timeout=*/2s, /*poll_interval=*/1ms,
      /*pump=*/[&pump_count]() { ++pump_count; });

  Expect(ok, "WaitUntil should succeed once pump has advanced the state enough");
  Expect(pump_count >= checks_needed, "pump must run before each predicate check");
}

// The value-returning usage pattern (wait on has_value, then read the value)
// used by the collapsed ad-hoc helpers works as intended.
void TestWaitUntilSupportsValueReadPattern() {
  std::atomic<int> produced{0};
  std::thread producer([&produced]() {
    std::this_thread::sleep_for(15ms);
    produced.store(42);
  });

  auto compute = [&produced]() -> std::optional<int> {
    const int value = produced.load();
    if (value == 0) {
      return std::nullopt;
    }
    return value;
  };

  const bool ready = WaitUntil([&compute]() { return compute().has_value(); },
                               /*timeout=*/2s, /*poll_interval=*/2ms);
  const std::optional<int> value = compute();
  producer.join();

  Expect(ready, "WaitUntil should observe the produced value");
  Expect(value.has_value() && *value == 42, "the awaited value should be readable after the wait");
}

}  // namespace

void RegisterTestSupportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TestSupport/WaitUntilReturnsPromptlyWhenPredicateFlips",
          TestWaitUntilReturnsPromptlyWhenPredicateFlips);
  AddTest(tests, "TestSupport/WaitUntilReturnsFalseOnTimeout",
          TestWaitUntilReturnsFalseOnTimeout);
  AddTest(tests, "TestSupport/WaitUntilRunsPumpEachIteration",
          TestWaitUntilRunsPumpEachIteration);
  AddTest(tests, "TestSupport/WaitUntilSupportsValueReadPattern",
          TestWaitUntilSupportsValueReadPattern);
}

}  // namespace microide::tests
