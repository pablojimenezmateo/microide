#pragma once

#include <cstdint>

namespace microide::workspace {

enum class StatusTone : std::uint8_t {
  Default = 0,
  Info,
  Warning,
  Error,
};

}
