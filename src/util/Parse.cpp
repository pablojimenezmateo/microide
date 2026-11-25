#include "util/Parse.h"

#include <charconv>
#include <cmath>

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

template <typename T>
std::optional<T> ParseRealExact(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const char* const begin = text.data();
  const char* const end = begin + text.size();

  T value{};
  const auto [ptr, ec] = std::from_chars(begin, end, value, std::chars_format::general);
  if (ptr != end) {
    return std::nullopt;
  }
  if (ec == std::errc::result_out_of_range) {
    long double wide{};
    const auto wide_result = std::from_chars(begin, end, wide, std::chars_format::general);
    if (wide_result.ec != std::errc{} || wide_result.ptr != end) {
      return std::nullopt;
    }
    value = static_cast<T>(wide);
  } else if (ec != std::errc{}) {
    return std::nullopt;
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

}

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
  return ParseRealExact<float>(text);
}

std::optional<double> ParseDouble(std::string_view text) {
  return ParseRealExact<double>(text);
}

int ParseIntOr(const std::optional<std::string>& text, int fallback) {
  return text.has_value() ? ParseInt(*text).value_or(fallback) : fallback;
}

}
