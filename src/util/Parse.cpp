#include "util/Parse.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace microide::util {

namespace {

template <typename T>
std::optional<T> ParseExact(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  T value{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<int> ParseInt(std::string_view text) {
  return ParseExact<int>(text);
}

std::optional<std::int64_t> ParseInt64(std::string_view text) {
  return ParseExact<std::int64_t>(text);
}

std::optional<std::size_t> ParseSize(std::string_view text) {
  return ParseExact<std::size_t>(text);
}

std::optional<float> ParseFloat(std::string_view text) {
  const std::optional<float> value = ParseExact<float>(text);
  if (!value.has_value() || !std::isfinite(*value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace microide::util
