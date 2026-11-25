#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

struct HoverTextMeasure {
  std::function<float(std::string_view text)> width;
  std::function<std::string(std::string_view text, float max_width)> truncate;
};

std::vector<std::string> WrapHoverText(std::string_view text, float max_width,
                                       std::size_t max_lines, const HoverTextMeasure& measure);

}
