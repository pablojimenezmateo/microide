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

void TestController::Clear() {
  test_items_.clear();
  results_.clear();
}

}  // namespace microide::workspace
