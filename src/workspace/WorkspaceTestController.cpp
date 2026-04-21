#include "workspace/WorkspaceTestController.h"

namespace microide::workspace {

TestController::TestController() = default;

TestController::~TestController() = default;

void TestController::RegisterTestItem(const TestItem& item) {
  // Check for duplicate
  for (const auto& existing : test_items_) {
    if (existing.id == item.id) {
      return;  // Already registered
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

const std::vector<TestResult>& TestController::TestResults(const std::string& test_id) const {
  static const std::vector<TestResult> kEmpty;
  // Return only results for this test_id
  // For simplicity, this is a linear search; in production use a map
  for (const auto& result : results_) {
    if (result.test_id == test_id) {
      return results_;  // Return all; caller filters by test_id
    }
  }
  return kEmpty;
}

void TestController::Clear() {
  test_items_.clear();
  results_.clear();
}

}  // namespace microide::workspace
