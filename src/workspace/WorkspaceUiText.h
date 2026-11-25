#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

#include "project/ProjectFileScanner.h"
#include "util/StringUtil.h"

namespace microide::workspace {

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

using util::AppendUnsigned;

inline void ComposeWindowTitle(std::string& out, std::string_view editor_title, bool dirty,
                               std::string_view project_label) {
  out.clear();
  if (!editor_title.empty()) {
    if (dirty) {
      out.append("\u25CF ");
    }
    out.append(editor_title).append(" \u2014 ");
  }
  if (!project_label.empty()) {
    out.append(project_label).append(" \u2014 ");
  }
  out.append("microide");
  (void)util::ReplaceInvalidUtf8WithReplacementCharacter(out);
}

inline std::string HomeAbbreviatedPath(const std::filesystem::path& path, std::string_view home) {
  std::string text = path.string();
  while (home.size() > 1 && home.back() == '/') {
    home.remove_suffix(1);
  }
  if (home.size() <= 1 || home.front() != '/' || text.compare(0, home.size(), home) != 0) {
    return text;
  }
  if (text.size() == home.size()) {
    return "~";
  }
  if (text[home.size()] != '/') {
    return text;
  }
  return "~" + text.substr(home.size());
}

inline constexpr std::string_view kHintSeparator = " · ";

inline void AppendHintSegment(std::string& line, std::string_view segment) {
  if (segment.empty()) {
    return;
  }
  if (!line.empty()) {
    line += kHintSeparator;
  }
  line.append(segment.data(), segment.size());
}

inline std::string JoinHintSegments(std::initializer_list<std::string_view> segments) {
  std::string result;
  for (std::string_view segment : segments) {
    AppendHintSegment(result, segment);
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

inline void AppendFilteredCountSummary(std::string& text,
                                       std::size_t shown,
                                       std::size_t total,
                                       std::string_view noun) {
  if (shown == 0) {
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

}
