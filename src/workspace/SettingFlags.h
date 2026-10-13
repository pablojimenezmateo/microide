#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StringUtil.h"

namespace microide::workspace {

// Single source of truth for interpreting a bool-typed setting value. A setting
// reads as enabled unless its value is one of the falsey tokens; an absent value
// yields `default_value`. Header-only and inline so the per-frame setting checks
// stay zero call overhead, and util::IsFalseyToken is allocation-free.
//
// The token set comes from util so the settings path and the env-var tracing
// switches cannot drift. It previously had its own case-SENSITIVE list that also
// omitted "no", so `FALSE` and `no` both read as ENABLED.
[[nodiscard]] inline bool SettingFlagEnabled(const std::optional<std::string>& value,
                                             bool default_value = false) {
  if (!value.has_value()) {
    return default_value;
  }
  return !util::IsFalseyToken(*value);
}

// Upper bounds for TD-2026-07-17A-106. `.gitignore` file loading skips files above
// 4 MiB, but the `project.files_exclude` setting path had no cap: a persisted or
// pasted setting could create an unbounded rule vector that is then copied into
// DirectoryTree/FileIndex/the native watcher and scanned per traversal predicate.
// Bound both the raw scanned bytes and the parsed rule count so the setting cannot
// amplify the per-directory matcher-copy cost or every watcher/scanner predicate.
inline constexpr std::size_t kMaxExcludeGlobsBytes = 256u * 1024;  // 256 KiB of setting text
inline constexpr std::size_t kMaxExcludeGlobsRules = 4096;

// Split the `project.files_exclude` setting into individual gitignore-style globs.
// Accepts newline- and/or comma-separated entries; trims whitespace and drops
// empty and comment (#) lines. Shared by the project-root wiring and the live
// settings-overlay re-apply so both parse identically. The raw text is scanned only
// up to `kMaxExcludeGlobsBytes` and at most `kMaxExcludeGlobsRules` globs are kept;
// when either cap trims input, `*truncated` (when provided) is set so callers can
// surface that the exclude set is incomplete.
// Calls `entry(std::string&&)` for each entry of a list-valued setting: newline-
// and/or comma-separated, whitespace trimmed, empty and comment (#) entries
// dropped. `entry` returns false to stop early. The one splitter every list
// setting uses, so they all accept the same spelling.
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

// `terminal.commands_to_skip_shell`: keybinding ids, `-id` to remove a default.
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

}  // namespace microide::workspace
