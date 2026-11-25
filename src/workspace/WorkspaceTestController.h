#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

struct TestItem {
  std::string id;
  std::string label;
  std::string file;
  int line = 0;
  std::string parent_id;
};

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

class TestController {
 public:
  TestController();
  ~TestController();

  void RegisterTestItem(const TestItem& item);

  const TestItem* FindTestItem(const std::string& id) const;

  const std::vector<TestItem>& TestItems() const { return test_items_; }

  void RecordTestResult(const TestResult& result);

  std::vector<TestResult> TestResults(const std::string& test_id) const;

  const TestResult* LatestTestResult(const std::string& test_id) const;

  void Clear();

 private:
  static constexpr std::size_t kMaxRetainedResults = 10000;

  std::vector<TestItem> test_items_;
  std::unordered_map<std::string, std::size_t> item_index_by_id_;
  std::deque<TestResult> results_;
  std::unordered_map<std::string, TestResult> latest_result_by_id_;
};

}
