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

void ParseSgrParametersInto(std::string_view body, std::vector<std::vector<int>>& groups) {
  // SGR fields are unsigned decimal only (no signs), so accumulate the integer
  // inline and clamp during accumulation to avoid overflow on hostile long runs.
  constexpr int kMaxParam = 65535;
  std::size_t group_count = 1;
  if (groups.empty()) {
    groups.emplace_back();
  }
  groups[0].clear();
  int value = 0;
  const auto flush_field = [&]() {
    groups[group_count - 1].push_back(std::min(value, kMaxParam));
    value = 0;
  };
  for (char character : body) {
    if (character == ';') {
      flush_field();
      ++group_count;
      if (groups.size() < group_count) {
        groups.emplace_back();
      }
      groups[group_count - 1].clear();
      continue;
    }
    if (character == ':') {
      flush_field();
      continue;
    }
    if (character >= '0' && character <= '9') {
      // Cap the running value so value*10 can never overflow int; the exact
      // saturated magnitude is irrelevant (downstream clamps to color ranges).
      if (value < 100000) {
        value = value * 10 + (character - '0');
      }
    }
  }
  flush_field();
  // Drop any stale groups left over from a previous, longer sequence while
  // keeping the outer vector's capacity (and the surviving inner vectors').
  groups.resize(group_count);
}

std::vector<std::vector<int>> ParseSgrParameters(std::string_view body) {
  std::vector<std::vector<int>> groups;
  ParseSgrParametersInto(body, groups);
  return groups;
}

}  // namespace microide::terminal
