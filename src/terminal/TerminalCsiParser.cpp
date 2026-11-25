#include "terminal/TerminalCsiParser.h"

#include <algorithm>

namespace microide::terminal {

void ParseCsiParametersInto(std::string_view body, std::vector<int>& params) {
  constexpr int kMaxParam = 65535;
  params.clear();
  bool field_active = false;
  bool negative = false;
  bool sign_allowed = true;
  int value = 0;
  const auto flush_field = [&]() {
    const int result = negative ? -value : value;
    params.push_back(std::clamp(result, -kMaxParam, kMaxParam));
    field_active = false;
    negative = false;
    sign_allowed = true;
    value = 0;
  };
  for (char character : body) {
    if (character == ';') {
      flush_field();
      continue;
    }
    if ((character == '-' || character == '+') && sign_allowed) {
      negative = (character == '-');
      field_active = true;
      sign_allowed = false;
      continue;
    }
    if (character >= '0' && character <= '9') {
      field_active = true;
      sign_allowed = false;
      if (value < 100000) {
        value = value * 10 + (character - '0');
      }
      continue;
    }
  }
  if (field_active || (!body.empty() && body.back() == ';')) {
    flush_field();
  }
}

std::vector<int> ParseCsiParameters(std::string_view body) {
  std::vector<int> params;
  ParseCsiParametersInto(body, params);
  return params;
}

int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback) {
  if (index >= params.size() || params[index] <= 0) {
    return fallback;
  }
  return params[index];
}

void ParseSgrParametersInto(std::string_view body, std::vector<std::vector<int>>& groups) {
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
      if (value < 100000) {
        value = value * 10 + (character - '0');
      }
    }
  }
  flush_field();
  groups.resize(group_count);
}

std::vector<std::vector<int>> ParseSgrParameters(std::string_view body) {
  std::vector<std::vector<int>> groups;
  ParseSgrParametersInto(body, groups);
  return groups;
}

}
