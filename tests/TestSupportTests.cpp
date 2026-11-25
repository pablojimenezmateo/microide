#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using namespace std::chrono_literals;

void TestWaitUntilReturnsPromptlyWhenPredicateFlips() {
  std::atomic<bool> ready{false};
  std::thread flip([&ready]() {
    std::this_thread::sleep_for(20ms);
    ready.store(true);
  });

  const auto start = std::chrono::steady_clock::now();
  const bool ok = WaitUntil([&ready]() { return ready.load(); },
                            2s, 2ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  flip.join();

  Expect(ok, "WaitUntil should observe the predicate becoming true");
  Expect(elapsed < 1s,
         "WaitUntil should return soon after the predicate flips, not sleep out the timeout");
}

void TestWaitUntilReturnsFalseOnTimeout() {
  const auto start = std::chrono::steady_clock::now();
  const bool ok = WaitUntil([]() { return false; },
                            60ms, 5ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  Expect(!ok, "WaitUntil should return false when the predicate never holds");
  Expect(elapsed >= 60ms, "WaitUntil should wait at least the full timeout before giving up");
}

void TestWaitUntilRunsPumpEachIteration() {
  int pump_count = 0;
  int checks_needed = 3;
  const bool ok = WaitUntil(
      [&pump_count, checks_needed]() { return pump_count >= checks_needed; },
      2s, 1ms,
      [&pump_count]() { ++pump_count; });

  Expect(ok, "WaitUntil should succeed once pump has advanced the state enough");
  Expect(pump_count >= checks_needed, "pump must run before each predicate check");
}

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
                               2s, 2ms);
  const std::optional<int> value = compute();
  producer.join();

  Expect(ready, "WaitUntil should observe the produced value");
  Expect(value.has_value() && *value == 42, "the awaited value should be readable after the wait");
}

void TestTimingWindowSeesAStarvedStretch() {
  std::ifstream pressure("/proc/pressure/cpu");
  if (!pressure.good()) {
    return;
  }
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency()) * 2;
  const TimingWindow window;
  std::atomic<bool> stop{false};
  std::vector<std::thread> spinners;
  for (unsigned i = 0; i < threads; ++i) {
    spinners.emplace_back([&stop]() {
      while (!stop.load(std::memory_order_relaxed)) {
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true);
  for (std::thread& spinner : spinners) {
    spinner.join();
  }
  Expect(window.StalledFraction() > 0.10 && !window.Trustworthy(),
         "a stretch with every core oversubscribed twice should read as starved: " + window.Describe());
}

}

void RegisterTestSupportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TestSupport/WaitUntilReturnsPromptlyWhenPredicateFlips",
          TestWaitUntilReturnsPromptlyWhenPredicateFlips);
  AddTest(tests, "TestSupport/WaitUntilReturnsFalseOnTimeout",
          TestWaitUntilReturnsFalseOnTimeout);
  AddTest(tests, "TestSupport/WaitUntilRunsPumpEachIteration",
          TestWaitUntilRunsPumpEachIteration);
  AddTest(tests, "TestSupport/TimingWindowSeesAStarvedStretch", TestTimingWindowSeesAStarvedStretch);
  AddTest(tests, "TestSupport/WaitUntilSupportsValueReadPattern",
          TestWaitUntilSupportsValueReadPattern);
}

}
