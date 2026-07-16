#include "workspace/WorkspaceTestController.h"

namespace microide::workspace {

TestController::TestController() = default;

TestController::~TestController() = default;

void TestController::RegisterTestItem(const TestItem& item) {
  // Upsert by id: a rediscovery with the same id but a new label/path/range must
  // UPDATE the existing item, not be dropped. Dropping it left stale navigation
  // and names after a test moved or was renamed.
  for (auto& existing : test_items_) {
    if (existing.id == item.id) {
      existing = item;
      return;
    }
  }
  test_items_.push_back(item);
}

const TestItem* TestController::FindTestItem(const std::string& id) const {
  for (const auto& item : test_items_) {
    if (item.id == id) {
      return &item;
    }
  }
  return nullptr;
}

void TestController::RecordTestResult(const TestResult& result) {
  results_.push_back(result);
  // Point the id at its newest result so the sidebar can look up the latest status in
  // O(1) without scanning the whole history.
  latest_result_index_by_id_[result.test_id] = results_.size() - 1;
}

std::vector<TestResult> TestController::TestResults(const std::string& test_id) const {
  std::vector<TestResult> filtered;
  for (const auto& result : results_) {
    if (result.test_id == test_id) {
      filtered.push_back(result);
    }
  }
  return filtered;
}

const TestResult* TestController::LatestTestResult(const std::string& test_id) const {
  const auto it = latest_result_index_by_id_.find(test_id);
  if (it == latest_result_index_by_id_.end() || it->second >= results_.size()) {
    return nullptr;
  }
  return &results_[it->second];
}

void TestController::Clear() {
  test_items_.clear();
  results_.clear();
  latest_result_index_by_id_.clear();
}

}  // namespace microide::workspace
