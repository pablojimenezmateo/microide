#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::util {

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset);
bool IsValidUtf8(std::string_view content);
std::vector<std::string> SplitLines(std::string_view content);

}  // namespace microide::util
