// util::Generation tests — the epoch-token staleness guard.

#include "TestSupport.h"

#include "util/Generation.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::util::Generation;

void TestStartsAtZero() {
  Generation gen;
  Expect(gen.current() == 0, "a fresh Generation starts at epoch 0");
  Expect(gen.is_current(0), "the initial token is current");
}

void TestBumpAdvancesAndReturnsNewToken() {
  Generation gen;
  const auto first = gen.bump();
  Expect(first == 1, "bump returns the new epoch");
  Expect(gen.current() == 1, "current reflects the bump");
  const auto second = gen.bump();
  Expect(second == 2, "bump advances monotonically");
}

void TestCapturedTokenGoesStaleAfterBump() {
  Generation gen;
  const auto captured = gen.current();
  Expect(gen.is_current(captured), "a freshly captured token is current");
  gen.bump();
  Expect(!gen.is_current(captured), "a captured token goes stale after a bump");
  const auto refreshed = gen.current();
  Expect(gen.is_current(refreshed), "re-capturing after the bump is current again");
}

}  // namespace

void RegisterGenerationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Generation/StartsAtZero", TestStartsAtZero);
  AddTest(tests, "Generation/BumpAdvancesAndReturnsNewToken", TestBumpAdvancesAndReturnsNewToken);
  AddTest(tests, "Generation/CapturedTokenGoesStaleAfterBump", TestCapturedTokenGoesStaleAfterBump);
}

}  // namespace microide::tests
