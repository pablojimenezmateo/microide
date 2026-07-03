#include "util/Parse.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

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

// Floating-point std::from_chars is not yet implemented in libc++ shipped
// with current Apple toolchains, so we route real-number parsing through
// strto* with a null-terminated copy. The program never installs a numeric
// locale, so the "C" locale's '.' decimal separator is used.
template <typename T, typename Convert>
std::optional<T> ParseRealExact(std::string_view text, Convert convert) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::string buffer(text);
  errno = 0;
  char* end = nullptr;
  const T value = convert(buffer.c_str(), &end);
  if (errno != 0) {
    return std::nullopt;
  }
  if (end != buffer.c_str() + buffer.size()) {
    return std::nullopt;
  }
  if (!std::isfinite(value)) {
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
  return ParseRealExact<float>(text, std::strtof);
}

std::optional<double> ParseDouble(std::string_view text) {
  return ParseRealExact<double>(text, std::strtod);
}

int ParseIntOr(const std::optional<std::string>& text, int fallback) {
  return text.has_value() ? ParseInt(*text).value_or(fallback) : fallback;
}

}  // namespace microide::util
