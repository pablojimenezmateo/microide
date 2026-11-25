#pragma once

#include <cstddef>

#include "util/JsonValue.h"

namespace microide::workspace {

struct CompareTabState;

struct ControlCompareQueryOptions {
  std::size_t max_rows = 20'000;
  std::size_t max_text_bytes = 4096;
};

util::JsonValue BuildControlCompareJson(const CompareTabState* compare,
                                        const ControlCompareQueryOptions& options = {});

}
