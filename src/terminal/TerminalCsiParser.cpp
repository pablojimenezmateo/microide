#include "terminal/TerminalCsiParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "util/Parse.h"

namespace microide::terminal {

namespace {

// Parse one accumulated CSI/SGR digit field. Out-of-range values are clamped to
// a generous bound (downstream clamps further to screen dimensions) so a hostile
// "CSI 99999999999 H" can never trigger std::atoi's undefined overflow.
int ParseCsiField(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  constexpr int kMaxParam = 65535;
  const std::optional<std::int64_t> value = util::ParseInt64(text);
  if (!value.has_value()) {
    // Overflowed int64 (an absurdly long digit run): treat as a very large
    // positive parameter rather than parsing garbage.
    return kMaxParam;
  }
  return static_cast<int>(std::clamp<std::int64_t>(*value, -kMaxParam, kMaxParam));
}

}  // namespace

std::vector<int> ParseCsiParameters(std::string_view body) {
  std::vector<int> params;
  std::string current;
  for (char character : body) {
    if (character == ';') {
      params.push_back(ParseCsiField(current));
      current.clear();
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(character)) ||
        ((character == '-' || character == '+') && current.empty())) {
      current.push_back(character);
      continue;
    }
  }
  if (!current.empty() || (!body.empty() && body.back() == ';')) {
    params.push_back(ParseCsiField(current));
  }
  return params;
}

int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback) {
  if (index >= params.size() || params[index] <= 0) {
    return fallback;
  }
  return params[index];
}

std::vector<std::vector<int>> ParseSgrParameters(std::string_view body) {
  std::vector<std::vector<int>> groups;
  groups.emplace_back();
  std::string current;
  const auto flush_field = [&]() {
    groups.back().push_back(ParseCsiField(current));
    current.clear();
  };
  for (char character : body) {
    if (character == ';') {
      flush_field();
      groups.emplace_back();
      continue;
    }
    if (character == ':') {
      flush_field();
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(character))) {
      current.push_back(character);
    }
  }
  flush_field();
  return groups;
}

}  // namespace microide::terminal
