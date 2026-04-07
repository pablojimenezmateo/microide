#include <string>
#include <vector>

namespace microide::diff_fixture {

struct StatusEntry {
  std::string label;
  int count = 0;
};

std::vector<StatusEntry> BuildEntries() {
  return {
      {"open", 3},
      {"saved", 7},
      {"closed", 1},
  };
}

int CountVisible(const std::vector<StatusEntry>& entries) {
  int total = 0;
  for (const StatusEntry& entry : entries) {
    if (entry.count > 0) {
      total += entry.count;
    }
  }
  return total;
}

std::string RenderSummary() {
  const auto entries = BuildEntries();
  return "visible=" + std::to_string(CountVisible(entries));
}

}  // namespace microide::diff_fixture
