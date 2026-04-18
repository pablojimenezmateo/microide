#include "workspace/WorkspaceProjectSearchPresentation.h"

namespace microide::workspace {

std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results) {
  std::vector<int> line_map;
  line_map.reserve(results.size() * 2);

  std::filesystem::path current_path;
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (result.relative_path != current_path) {
      current_path = result.relative_path;
      line_map.push_back(-1);
    }
    line_map.push_back(static_cast<int>(i));
  }

  return line_map;
}

int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index) {
  for (std::size_t line = 0; line < line_map.size(); ++line) {
    if (line_map[line] == static_cast<int>(result_index)) {
      return static_cast<int>(line);
    }
  }
  return 0;
}

}  // namespace microide::workspace
