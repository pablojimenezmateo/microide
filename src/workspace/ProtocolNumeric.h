#pragma once

#include <cstdint>
#include <limits>

#include "util/JsonValue.h"

// Shared numeric coercion for the LSP and DAP wire parsers. Both protocols read
// integer fields (positions, ids, thread/frame refs, counts) that arrive as a
// `JsonValue`. `JsonValue::AsInt()` returns a 64-bit value clamped to the int64
// range, but the protocol structs store `int`, and a bare `static_cast<int>` of
// an out-of-int-range value is implementation-defined ([conv.integral]) — a
// hostile/buggy server can wrap `INT64_MAX` into a small or negative id and steer
// the client to the wrong thread/frame/edit location.
//
// `JsonIntInRange` narrows deterministically: values outside `[INT_MIN, INT_MAX]`
// are clamped to the nearest int bound (never wrapped), so a huge positive id
// stays a large positive int and a huge negative id stays negative. Callers that
// reject negatives do so on the clamped, sign-preserving result.
namespace microide::workspace::protocol_numeric {

inline int JsonIntInRange(const util::JsonValue& value, int fallback = 0) {
  const std::int64_t raw = value.AsInt(fallback);
  if (raw < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  if (raw > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(raw);
}

}  // namespace microide::workspace::protocol_numeric
