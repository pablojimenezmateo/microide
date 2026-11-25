#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace microide::workspace {

struct StdioQueuedMessage {
  std::string serialized;
  std::function<std::string()> build_serialized;
  std::size_t approx_bytes = 0;

  std::string TakeSerialized() && {
    if (!serialized.empty()) {
      return std::move(serialized);
    }
    return build_serialized ? build_serialized() : std::string{};
  }
};

}
