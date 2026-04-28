#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace microide::util {

std::optional<int> ParseInt(std::string_view text);
std::optional<std::int64_t> ParseInt64(std::string_view text);
std::optional<std::size_t> ParseSize(std::string_view text);
std::optional<float> ParseFloat(std::string_view text);

}  // namespace microide::util
