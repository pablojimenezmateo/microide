#pragma once

#include <vector>

#include "project/ProjectSearchService.h"

namespace microide::workspace {

std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results);
int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index);

}  // namespace microide::workspace
