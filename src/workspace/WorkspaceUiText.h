#pragma once

#include <array>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>

#include "project/ProjectFileScanner.h"

namespace microide::workspace {

// A short, cause-specific note for an incomplete file-index scan, or an empty
// view when the scan was complete (TD-2026-07-17-008/033). Returns a static
// literal so render callers never materialize a string. Precedence favors the
// most consequential cause (too large > too deep > unreadable folders > error).
inline std::string_view ScanIncompleteNote(const project::ProjectFileScanStatus& status) {
  if (status.truncated_by_budget) {
    return "index incomplete — project too large";
  }
  if (status.truncated_by_depth) {
    return "index incomplete — tree too deep";
  }
  if (status.permission_limited) {
    return "index incomplete — some folders unreadable";
  }
  if (status.error) {
    return "index incomplete";
  }
  return {};
}

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
      // Same separator the overlay key hints use ("↑↓ select · Enter choose ·
      // Esc cancel"), so the shell spells a hint list one way.
      result += " · ";
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

// Result-count line for any query-filtered list: "12 of 187 commands" while the
// query is excluding rows, "187 commands" when it is not, "No matching commands"
// when nothing survived. Shared by the quick-open pickers and the Settings /
// Help-About footer so a count means the same thing everywhere.
//
// The denominator is deliberately dropped when it equals the numerator: an
// unfiltered palette read "187 of 187", which is noise that also looks like a
// filter is active. A bare "0 of 187" reads worse than saying nothing matched.
// Appending form, for the render view-model builder's reused scratch string: the
// overlay chrome composes into a frame-stable buffer specifically to keep painting
// allocation-free, so it must not take a freshly-built std::string per frame.
inline void AppendFilteredCountSummary(std::string& text,
                                       std::size_t shown,
                                       std::size_t total,
                                       std::string_view noun) {
  if (shown == 0) {
    text += "No matching ";
    text.append(noun.data(), noun.size());
    return;
  }
  AppendUnsigned(text, shown);
  if (shown < total) {
    text += " of ";
    AppendUnsigned(text, total);
  }
  text.push_back(' ');
  text.append(noun.data(), noun.size());
}

inline std::string BuildFilteredCountSummary(std::size_t shown,
                                             std::size_t total,
                                             std::string_view noun) {
  std::string text;
  text.reserve(28 + noun.size());
  AppendFilteredCountSummary(text, shown, total, noun);
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

// Builds "Showing <shown> of <total> matches" + suffix. Used when a count-all
// project search knows the exact total but only displays the first `shown`.
inline std::string BuildShownOfTotalStatus(std::size_t shown,
                                           std::size_t total,
                                           std::string_view suffix) {
  std::string text;
  text.reserve(40 + suffix.size());
  text += "Showing ";
  AppendUnsigned(text, shown);
  text += " of ";
  AppendUnsigned(text, total);
  text += " matches";
  text.append(suffix.data(), suffix.size());
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
