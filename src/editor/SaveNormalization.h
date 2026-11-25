#pragma once

#include <string>
#include <vector>

namespace microide::editor {

bool TrimTrailingWhitespace(std::vector<std::string>& lines);

bool EnsureSingleFinalNewline(std::vector<std::string>& lines);

}
