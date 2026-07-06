#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StringUtil.h"

namespace microide::workspace {

// Single source of truth for interpreting a bool-typed setting value. A setting
// reads as enabled unless its value is one of the falsey tokens ("false", "0",
// or "off"); an absent value yields `default_value`. Header-only and inline so
// the per-frame setting checks stay zero call overhead.
[[nodiscard]] inline bool SettingFlagEnabled(const std::optional<std::string>& value,
                                             bool default_value = false) {
  if (!value.has_value()) {
    return default_value;
  }
  return *value != "false" && *value != "0" && *value != "off";
}

// Split the `project.files_exclude` setting into individual gitignore-style globs.
// Accepts newline- and/or comma-separated entries; trims whitespace and drops
// empty and comment (#) lines. Shared by the project-root wiring and the live
// settings-overlay re-apply so both parse identically.
[[nodiscard]] inline std::vector<std::string> ParseExcludeGlobs(std::string_view text) {
  std::vector<std::string> globs;
  for (const std::string_view line : util::SplitLineViews(text)) {
    std::size_t start = 0;
    while (start <= line.size()) {
      const std::size_t comma = line.find(',', start);
      const std::string_view piece = comma == std::string_view::npos
                                         ? line.substr(start)
                                         : line.substr(start, comma - start);
      std::string trimmed = util::TrimAsciiWhitespace(piece);
      if (!trimmed.empty() && trimmed.front() != '#') {
        globs.push_back(std::move(trimmed));
      }
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }
  }
  return globs;
}

}  // namespace microide::workspace
