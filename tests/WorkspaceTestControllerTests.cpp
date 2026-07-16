#include "TestSupport.h"

#include "workspace/WorkspaceTestController.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::TestController;
using microide::workspace::TestItem;
using microide::workspace::TestResult;
using microide::workspace::TestResultState;

// Regression: RegisterTestItem must UPSERT by id. A rediscovery with the same id
// but a new label/file/line (a renamed or moved test) previously dropped the new
// data, leaving stale navigation and names.
void TestRegisterTestItemUpsertsById() {
  TestController controller;
  controller.RegisterTestItem(TestItem{.id = "t1", .label = "old", .file = "a.cpp", .line = 3});
  const TestItem* first = controller.FindTestItem("t1");
  Expect(first != nullptr && first->label == "old" && first->line == 3,
         "the initial registration is recorded");

  // Re-register the same id with new metadata: it must update in place.
  controller.RegisterTestItem(TestItem{.id = "t1", .label = "new", .file = "b.cpp", .line = 42});
  const TestItem* updated = controller.FindTestItem("t1");
  Expect(updated != nullptr && updated->label == "new" && updated->file == "b.cpp" &&
             updated->line == 42,
         "re-registering the same id updates its label/file/line");

  // A different id adds a second item rather than replacing.
  controller.RegisterTestItem(TestItem{.id = "t2", .label = "second"});
  Expect(controller.FindTestItem("t2") != nullptr && controller.FindTestItem("t1") != nullptr,
         "a distinct id is added alongside the first");
}

// Regression: TestResults returns by value, so results for two different tests
// held simultaneously do not alias a shared scratch buffer.
void TestResultsDoNotAliasAcrossCalls() {
  TestController controller;
  controller.RecordTestResult(TestResult{.test_id = "a", .state = TestResultState::Passed});
  controller.RecordTestResult(TestResult{.test_id = "b", .state = TestResultState::Failed});

  const auto a = controller.TestResults("a");
  const auto b = controller.TestResults("b");
  Expect(a.size() == 1 && a.front().state == TestResultState::Passed,
         "results for 'a' are its own");
  Expect(b.size() == 1 && b.front().state == TestResultState::Failed,
         "results for 'b' are not clobbered by the second query");
}

// TD-2026-07-16-66: LatestTestResult returns the most recent result per id in O(1),
// nullptr when none, matching TestResults(...).back() without the per-call allocation.
void TestLatestTestResultReturnsMostRecent() {
  TestController controller;
  Expect(controller.LatestTestResult("a") == nullptr, "no result yet -> nullptr");

  controller.RecordTestResult(TestResult{.test_id = "a", .state = TestResultState::Failed});
  controller.RecordTestResult(TestResult{.test_id = "b", .state = TestResultState::Passed});
  controller.RecordTestResult(TestResult{.test_id = "a", .state = TestResultState::Passed});

  const TestResult* a = controller.LatestTestResult("a");
  Expect(a != nullptr && a->state == TestResultState::Passed,
         "LatestTestResult returns the newest result for 'a' (last-writer-wins)");
  const TestResult* b = controller.LatestTestResult("b");
  Expect(b != nullptr && b->state == TestResultState::Passed, "'b' latest is its only result");

  controller.Clear();
  Expect(controller.LatestTestResult("a") == nullptr, "Clear resets the latest-result index");
}

}  // namespace

void RegisterWorkspaceTestControllerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceTestController/RegisterTestItemUpsertsById",
          TestRegisterTestItemUpsertsById);
  AddTest(tests, "WorkspaceTestController/ResultsDoNotAliasAcrossCalls",
          TestResultsDoNotAliasAcrossCalls);
  AddTest(tests, "WorkspaceTestController/LatestTestResultReturnsMostRecent",
          TestLatestTestResultReturnsMostRecent);
}

}  // namespace microide::tests
