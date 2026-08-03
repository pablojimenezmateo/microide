#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "util/StringUtil.h"

namespace microide::workspace {

// The settings `ApplyEditorPreferences` reads, resolved once.
//
// Every one of these is shell-global, not per-viewport — yet they were read
// inside the per-tab loop, so `ApplyEditorPreferencesToAllTabs` was
// O(tabs x settings) in lookups. Each lookup returns `std::optional<std::string>`
// by value, so each one also *allocated*: ~5 allocations per viewport on the
// cheap path and ~8 on the contract path, repeated for every open tab of every
// settings change, project activation and session restore.
//
// Resolving once and passing the result down makes that O(tabs + settings).
// `settings_change_many_tabs` is the perf scenario that gates it.
struct EditorPreferenceSettings {
  bool editorconfig_enabled = true;
  bool trim_trailing_whitespace = true;
  bool ensure_final_newline = true;
  // Resolved from `editor.line_endings`: nullopt means "auto" (keep the file's
  // detected ending). Resolved here so the string compare is done once, not once
  // per viewport.
  std::optional<util::LineEnding> save_line_ending;
  // Contract family — only consulted when a language contract is rebuilt.
  bool auto_close = true;
  bool surround = true;
  bool smart_indent = true;
};

// Resolve the snapshot through `get_setting`. Deliberately a free function over a
// getter rather than a shell method: it is pure, so it is directly testable
// without a shell.
EditorPreferenceSettings ResolveEditorPreferenceSettings(
    const std::function<std::optional<std::string>(std::string_view)>& get_setting);

}  // namespace microide::workspace
