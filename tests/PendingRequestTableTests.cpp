#include "TestSupport.h"

#include <chrono>
#include <functional>
#include <vector>

#include "workspace/PendingRequestTable.h"

namespace microide::tests {
namespace {

using Table = workspace::PendingRequestTable<std::function<int()>>;
using std::chrono::milliseconds;

void TestEachEntryIsClaimedOnce() {
  Table table;
  table.Add(1, [] { return 1; }, milliseconds(1000));
  table.Add(2, [] { return 2; }, milliseconds(1000));
  const auto answered = table.Take(1);
  Expect(answered.has_value() && answered->callback() == 1, "a response takes its own entry");
  Expect(!table.Take(1).has_value(), "a second response for the same id finds nothing");
  Expect(table.Remove(2), "a failed send removes an unclaimed entry");
  Expect(!table.Remove(2), "and reports a claimed one, so the failure is not delivered twice");
  Expect(table.empty(), "nothing is left");
}

void TestSweepFailsOnlyExpiredUnlessThePeerIsGone() {
  Table table;
  table.Add(1, [] { return 1; }, milliseconds(0));
  table.Add(2, [] { return 2; }, milliseconds(60'000));
  std::vector<int> failed;
  const auto now = Table::Clock::now() + milliseconds(1);
  Expect(table.Sweep(/*only_expired=*/true, now, [&](int id, Table::Entry) { failed.push_back(id); }) == 1 &&
             failed == std::vector<int>{1},
         "the timeout pass fails only the expired request");
  Expect(table.Sweep(/*only_expired=*/false, now, [&](int id, Table::Entry) { failed.push_back(id); }) == 1 &&
             failed == std::vector<int>{1, 2},
         "the peer-gone pass fails the rest");
  Expect(table.empty() && !table.Take(2).has_value(), "a swept entry cannot also be answered");
}

}  // namespace

void RegisterPendingRequestTableTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PendingRequestTable/EachEntryIsClaimedOnce", TestEachEntryIsClaimedOnce);
  AddTest(tests, "PendingRequestTable/SweepFailsOnlyExpiredUnlessThePeerIsGone",
          TestSweepFailsOnlyExpiredUnlessThePeerIsGone);
}

}  // namespace microide::tests
