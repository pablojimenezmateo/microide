#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace microide::util {

struct TransparentStringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};

}
