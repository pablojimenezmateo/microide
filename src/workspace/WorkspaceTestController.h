#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Test item: a test case or test suite that can be run.
struct TestItem {
  std::string id;
  std::string label;
  std::string file;
  int line = 0;
  std::string parent_id;  // for nested test suites
};

// Test run result.
enum class TestResultState {
  Queued,
  InProgress,
  Passed,
  Failed,
  Skipped,
  Errored,
};

struct TestResult {
  std::string test_id;
  TestResultState state = TestResultState::Queued;
  std::string message;
  int duration_ms = 0;
};

// Test controller: registry and runtime for test discovery and execution.
class TestController {
 public:
  TestController();
  ~TestController();

  // Register a test item.
  void RegisterTestItem(const TestItem& item);

  // Find test item by id.
  const TestItem* FindTestItem(const std::string& id) const;

  // Get all test items.
  const std::vector<TestItem>& TestItems() const { return test_items_; }

  // Record a test result.
  void RecordTestResult(const TestResult& result);

  // Get results for a test item. Returns by value: a previous implementation
  // returned a reference to a reused scratch member, so two back-to-back calls
  // aliased the same storage and a caller holding both references read the wrong
  // test's results.
  std::vector<TestResult> TestResults(const std::string& test_id) const;

  // Clear all test items and results.
  void Clear();

 private:
  std::vector<TestItem> test_items_;
  std::vector<TestResult> results_;
};

}  // namespace microide::workspace
