#include "terminal/TerminalCsiParser.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace microide::terminal {

std::vector<int> ParseCsiParameters(std::string_view body) {
  std::vector<int> params;
  std::string current;
  for (char character : body) {
    if (character == ';') {
      params.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
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
    params.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
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
    groups.back().push_back(current.empty() ? 0 : std::atoi(current.c_str()));
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
