#pragma once

#include <cstddef>
#include <deque>
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
  // Bound on retained run history. Discovery upserts and the sidebar read only the
  // latest per-id result (kept separately, uncapped-per-id but bounded by test
  // count), so the full run log is a bounded FIFO: repeated runs can no longer grow
  // results_ without limit or make TestResults(id) scan ever more stale rows.
  static constexpr std::size_t kMaxRetainedResults = 10000;

  std::vector<TestItem> test_items_;
  // id -> index into test_items_ for O(1) upsert/lookup (discovery of N unique ids
  // was O(N^2) with the old linear scan per RegisterTestItem/FindTestItem).
  std::unordered_map<std::string, std::size_t> item_index_by_id_;
  // Bounded FIFO of every recorded result (oldest evicted past kMaxRetainedResults).
  std::deque<TestResult> results_;
  // id -> that test's MOST RECENT result (last-writer-wins). Held separately from the
  // bounded history so the sidebar's latest-status lookup stays O(1) and survives
  // history eviction. Bounded by the number of distinct tests (itself discovery-capped).
  std::unordered_map<std::string, TestResult> latest_result_by_id_;
};

}  // namespace microide::workspace
