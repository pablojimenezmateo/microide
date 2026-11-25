#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

std::optional<std::string> ZstdDelta(std::string_view base, std::string_view target);

std::optional<std::string> ZstdApplyDelta(std::string_view base, std::string_view delta,
                                          std::size_t max_size);

}
