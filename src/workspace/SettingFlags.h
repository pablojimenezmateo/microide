#pragma once

#include <optional>
#include <string>

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

}  // namespace microide::workspace
