#pragma once

#include <string_view>
#include <vector>

namespace microide::terminal {

std::vector<int> ParseCsiParameters(std::string_view body);
int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback);

// SGR parameters can carry ITU T.416 colon-separated sub-parameters
// (e.g. `38:2:255:0:0` for direct color or `4:3` for curly underline) in
// addition to the legacy semicolon form. Each returned group is one
// semicolon-delimited parameter, split into its colon sub-parameters. An empty
// numeric field decodes to 0 (the SGR "default" placeholder).
std::vector<std::vector<int>> ParseSgrParameters(std::string_view body);

}  // namespace microide::terminal
