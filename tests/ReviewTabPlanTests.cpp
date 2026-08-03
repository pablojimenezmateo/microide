#include "TestSupport.h"

#include <filesystem>
#include <vector>

#include "workspace/git/ReviewTabPlan.h"

namespace microide::tests {
namespace {

using microide::workspace::ComputeReviewTabPlan;
using microide::workspace::ReviewTabPlan;
using microide::workspace::ReviewTabRef;

std::filesystem::path P(const char* p) { return std::filesystem::path(p); }

void TestReviewPlanOpensAllNewTargets() {
  const std::vector<ReviewTabRef> existing;
  const std::vector<std::filesystem::path> targets{P("a.cpp"), P("b.cpp")};
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.to_open.size() == 2, "all targets open when nothing is already open");
  Expect(plan.reused.empty(), "nothing reused when nothing open");
  Expect(plan.to_close.empty(), "nothing to close when nothing open");
  Expect(plan.to_open[0] == P("a.cpp") && plan.to_open[1] == P("b.cpp"),
         "target order preserved");
}

void TestReviewPlanDedupsAlreadyOpenTargets() {
  const std::vector<ReviewTabRef> existing{{P("a.cpp"), 0, false}};
  const std::vector<std::filesystem::path> targets{P("a.cpp"), P("b.cpp")};
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.reused.size() == 1 && plan.reused[0] == P("a.cpp"), "open target reused");
  Expect(plan.to_open.size() == 1 && plan.to_open[0] == P("b.cpp"), "only the new target opens");
  Expect(plan.to_close.empty(), "a still-wanted tab is not closed");
}

void TestReviewPlanClosesStaleCleanTabs() {
  const std::vector<ReviewTabRef> existing{{P("a.cpp"), 0, false}, {P("b.cpp"), 1, false}};
  const std::vector<std::filesystem::path> targets{P("a.cpp")};
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.reused.size() == 1 && plan.reused[0] == P("a.cpp"), "wanted tab reused");
  Expect(plan.to_close.size() == 1 && plan.to_close[0] == 1, "stale clean tab queued to close");
  Expect(plan.kept_dirty.empty(), "no dirty tabs to keep");
}

void TestReviewPlanKeepsStaleDirtyTabs() {
  const std::vector<ReviewTabRef> existing{{P("a.cpp"), 0, false}, {P("b.cpp"), 1, true}};
  const std::vector<std::filesystem::path> targets;
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.to_close.size() == 1 && plan.to_close[0] == 0, "stale clean tab closes");
  Expect(plan.kept_dirty.size() == 1 && plan.kept_dirty[0] == P("b.cpp"),
         "stale dirty tab is preserved, never closed");
}

void TestReviewPlanClosesInDescendingIndexOrder() {
  const std::vector<ReviewTabRef> existing{
      {P("a.cpp"), 0, false}, {P("b.cpp"), 1, false}, {P("c.cpp"), 2, false}};
  const std::vector<std::filesystem::path> targets;
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.to_close.size() == 3, "all stale clean tabs close");
  Expect(plan.to_close[0] == 2 && plan.to_close[1] == 1 && plan.to_close[2] == 0,
         "close indices are descending so the caller can close without re-indexing");
}

void TestReviewPlanCollapsesDuplicateTargets() {
  const std::vector<ReviewTabRef> existing;
  const std::vector<std::filesystem::path> targets{P("a.cpp"), P("a.cpp")};
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.to_open.size() == 1 && plan.to_open[0] == P("a.cpp"),
         "duplicate targets collapse to a single open");
}

void TestReviewPlanEmptyInputsProduceEmptyPlan() {
  const std::vector<ReviewTabRef> existing;
  const std::vector<std::filesystem::path> targets;
  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);
  Expect(plan.to_open.empty() && plan.reused.empty() && plan.to_close.empty() &&
             plan.kept_dirty.empty(),
         "empty inputs yield an empty plan");
}

}  // namespace

void RegisterReviewTabPlanTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ReviewTabPlan/OpensAllNewTargets", TestReviewPlanOpensAllNewTargets);
  AddTest(tests, "ReviewTabPlan/DedupsAlreadyOpenTargets", TestReviewPlanDedupsAlreadyOpenTargets);
  AddTest(tests, "ReviewTabPlan/ClosesStaleCleanTabs", TestReviewPlanClosesStaleCleanTabs);
  AddTest(tests, "ReviewTabPlan/KeepsStaleDirtyTabs", TestReviewPlanKeepsStaleDirtyTabs);
  AddTest(tests, "ReviewTabPlan/ClosesInDescendingIndexOrder",
          TestReviewPlanClosesInDescendingIndexOrder);
  AddTest(tests, "ReviewTabPlan/CollapsesDuplicateTargets", TestReviewPlanCollapsesDuplicateTargets);
  AddTest(tests, "ReviewTabPlan/EmptyInputsProduceEmptyPlan",
          TestReviewPlanEmptyInputsProduceEmptyPlan);
}

}  // namespace microide::tests
