#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include "util/JsonValue.h"

namespace microide::workspace::protocol_numeric {

inline bool IsIntegralJsonNumber(const util::JsonValue& value) {
  if (value.IsInt()) {
    return true;
  }
  if (value.IsDouble()) {
    const double raw = value.AsDouble();
    return std::isfinite(raw) && std::trunc(raw) == raw;
  }
  return false;
}

[[gnu::noinline]] inline int JsonIntInRangeSlow(const util::JsonValue& value, int fallback) {
  if (value.IsDouble() && !IsIntegralJsonNumber(value)) {
    return fallback;
  }
  const std::int64_t raw = value.AsInt(fallback);
  if (raw < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  if (raw > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(raw);
}

[[gnu::always_inline]] inline int JsonIntInRange(const util::JsonValue& value, int fallback = 0) {
  if (const auto* n = std::get_if<std::int64_t>(&value.v)) [[likely]] {
    if (*n < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
      return std::numeric_limits<int>::min();
    }
    if (*n > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
      return std::numeric_limits<int>::max();
    }
    return static_cast<int>(*n);
  }
  return JsonIntInRangeSlow(value, fallback);
}

}
