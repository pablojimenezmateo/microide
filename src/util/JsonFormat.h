#pragma once

#include <cstddef>
#include <string>
#include <string_view>  // IWYU pragma: keep

namespace microide::util {

struct JsonFormatResult {
  bool ok = false;
  std::string text;
  std::size_t error_offset = 0;
  std::string error;
};

JsonFormatResult FormatJson(std::string_view src, std::string_view indent_unit);

}
