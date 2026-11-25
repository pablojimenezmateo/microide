#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace microide::util {

std::vector<std::string> SplitCommandLine(std::string_view text);

}
