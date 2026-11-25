#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>

namespace microide::workspace {

struct OutputReference {
  std::filesystem::path path;
  std::size_t line = 0;
  std::size_t column = 0;
};

std::optional<OutputReference> ParseOutputReference(std::string_view text);

}
