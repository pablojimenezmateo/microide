#pragma once

#include <string_view>
#include <vector>

namespace microide::terminal {

std::vector<int> ParseCsiParameters(std::string_view body);
int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback);

}  // namespace microide::terminal
