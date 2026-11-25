#pragma once

#include <string_view>
#include <vector>

namespace microide::terminal {

std::vector<int> ParseCsiParameters(std::string_view body);
void ParseCsiParametersInto(std::string_view body, std::vector<int>& params);
int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback);

std::vector<std::vector<int>> ParseSgrParameters(std::string_view body);

void ParseSgrParametersInto(std::string_view body, std::vector<std::vector<int>>& groups);

}
