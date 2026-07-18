#include "workspace/WorkspaceTestController.h"

namespace microide::workspace {

TestController::TestController() = default;

TestController::~TestController() = default;

void TestController::RegisterTestItem(const TestItem& item) {
  // Upsert by id in O(1): a rediscovery with the same id but a new label/path/range
  // must UPDATE the existing item, not be dropped (dropping it left stale navigation
  // and names after a test moved or was renamed). The id->index map keeps discovery
  // of N unique tests O(N) instead of the old O(N^2) linear scan per registration.
  const auto [it, inserted] = item_index_by_id_.try_emplace(item.id, test_items_.size());
  if (inserted) {
    test_items_.push_back(item);
  } else {
    test_items_[it->second] = item;
  }
}

const TestItem* TestController::FindTestItem(const std::string& id) const {
  const auto it = item_index_by_id_.find(id);
  return it == item_index_by_id_.end() ? nullptr : &test_items_[it->second];
}

void TestController::RecordTestResult(const TestResult& result) {
  // Latest-per-id (last-writer-wins) is stored separately so the sidebar's O(1)
  // status lookup survives history eviction below.
  latest_result_by_id_[result.test_id] = result;
  results_.push_back(result);
  if (results_.size() > kMaxRetainedResults) {
    results_.pop_front();  // bounded FIFO: drop the oldest run record
  }
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
  const auto it = latest_result_by_id_.find(test_id);
  return it == latest_result_by_id_.end() ? nullptr : &it->second;
}

void TestController::Clear() {
  test_items_.clear();
  item_index_by_id_.clear();
  results_.clear();
  latest_result_by_id_.clear();
}

}  // namespace microide::workspace
