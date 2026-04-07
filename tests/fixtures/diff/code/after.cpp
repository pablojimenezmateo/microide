#include <string>
#include <vector>

namespace microide::diff_fixture {

struct StatusEntry {
  std::string label;
  int count = 0;
  bool pinned = false;
};

std::vector<StatusEntry> BuildEntries() {
  return {
      {"open", 3, true},
      {"saved", 8, false},
      {"staged", 2, true},
  };
}

int CountVisible(const std::vector<StatusEntry>& entries) {
  int total = 0;
  for (const StatusEntry& entry : entries) {
    if (entry.count > 0 && !entry.label.empty()) {
      total += entry.count;
    }
  }
  return total;
}

std::string RenderSummary() {
  const auto entries = BuildEntries();
  return "visible=" + std::to_string(CountVisible(entries)) + ",first=" + entries.front().label;
}

}  // namespace microide::diff_fixture
