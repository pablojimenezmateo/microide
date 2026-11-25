#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StringUtil.h"

namespace microide::workspace {

[[nodiscard]] inline bool SettingFlagEnabled(const std::optional<std::string>& value,
                                             bool default_value = false) {
  if (!value.has_value()) {
    return default_value;
  }
  return !util::IsFalseyToken(*value);
}

inline constexpr std::size_t kMaxExcludeGlobsBytes = 256u * 1024;
inline constexpr std::size_t kMaxExcludeGlobsRules = 4096;

template <typename Entry>
inline void ForEachSettingListEntry(std::string_view text, Entry&& entry) {
  for (const std::string_view line : util::SplitLineViews(text)) {
    std::size_t start = 0;
    while (start <= line.size()) {
      const std::size_t comma = line.find(',', start);
      const std::string_view piece = comma == std::string_view::npos
                                         ? line.substr(start)
                                         : line.substr(start, comma - start);
      std::string trimmed = util::TrimAsciiWhitespace(piece);
      if (!trimmed.empty() && trimmed.front() != '#' && !entry(std::move(trimmed))) {
        return;
      }
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }
  }
}

[[nodiscard]] inline std::vector<std::string> ParseExcludeGlobs(std::string_view text,
                                                                bool* truncated = nullptr) {
  std::vector<std::string> globs;
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (text.size() > kMaxExcludeGlobsBytes) {
    text = text.substr(0, kMaxExcludeGlobsBytes);
    if (truncated != nullptr) {
      *truncated = true;
    }
  }
  ForEachSettingListEntry(text, [&](std::string&& glob) {
    if (globs.size() >= kMaxExcludeGlobsRules) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      return false;
    }
    globs.push_back(std::move(glob));
    return true;
  });
  return globs;
}

[[nodiscard]] inline std::vector<std::string> ParseSkipShellList(
    const std::optional<std::string>& value) {
  std::vector<std::string> ids;
  if (value.has_value()) {
    ForEachSettingListEntry(*value, [&](std::string&& id) {
      ids.push_back(std::move(id));
      return true;
    });
  }
  return ids;
}

}
