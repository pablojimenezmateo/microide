#pragma once

#include <array>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>

namespace microide::workspace {

inline void AppendUnsigned(std::string& out, std::size_t value) {
  std::array<char, 20> scratch;
  const auto [end, ec] =
      std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
  if (ec == std::errc{}) {
    out.append(scratch.data(), static_cast<std::size_t>(end - scratch.data()));
  }
}

inline std::string JoinHintSegments(std::initializer_list<std::string_view> segments) {
  std::string result;
  bool first = true;
  for (std::string_view segment : segments) {
    if (segment.empty()) {
      continue;
    }
    if (!first) {
      result += "  |  ";
    }
    result.append(segment.data(), segment.size());
    first = false;
  }
  return result;
}

inline std::string FormatEmptyState(std::string_view noun, std::string_view qualifier = {}) {
  std::string text = "No ";
  text.append(noun.data(), noun.size());
  if (!qualifier.empty()) {
    text.push_back(' ');
    text.append(qualifier.data(), qualifier.size());
  }
  return text;
}

inline std::string BuildCountStatus(std::string_view prefix,
                                    std::size_t count,
                                    std::string_view suffix) {
  std::string text;
  text.reserve(prefix.size() + suffix.size() + 24);
  text += prefix;
  AppendUnsigned(text, count);
  text += suffix;
  return text;
}

// Appends a " (X of Y files)" denominator clause when total > 0. Used by the
// project search sidebar/overlay to surface progress on large repos so the
// "Searching N matches" readout has a denominator while the worker is active.
inline std::string BuildSearchProgressSuffix(std::size_t searched, std::size_t total) {
  if (total == 0) {
    return {};
  }
  std::string text;
  text.reserve(24);
  text += " (";
  AppendUnsigned(text, searched);
  text += " of ";
  AppendUnsigned(text, total);
  text += " files)";
  return text;
}

inline std::string BuildSelectionSummary(std::size_t selected,
                                         std::size_t total,
                                         std::string_view suffix) {
  std::string summary;
  summary.reserve(48 + suffix.size());
  AppendUnsigned(summary, selected + 1);
  summary += " / ";
  AppendUnsigned(summary, total);
  summary += suffix;
  return summary;
}

}  // namespace microide::workspace
