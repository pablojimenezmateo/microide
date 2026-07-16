#pragma once

#include <string>
#include <unordered_map>
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

  // O(1) latest-result lookup for the Tests sidebar, which only needs each test's most
  // recent status. Returns nullptr when the test has no recorded result. Avoids the
  // per-row filtered-vector allocation + full-history scan that `TestResults(id).back()`
  // paid once per discovered test on every sidebar rebuild. (TD-2026-07-16-66.)
  const TestResult* LatestTestResult(const std::string& test_id) const;

  // Clear all test items and results.
  void Clear();

 private:
  std::vector<TestItem> test_items_;
  std::vector<TestResult> results_;
  // test_id -> index into results_ of that test's MOST RECENT result (last-writer-wins).
  std::unordered_map<std::string, std::size_t> latest_result_index_by_id_;
};

}  // namespace microide::workspace
