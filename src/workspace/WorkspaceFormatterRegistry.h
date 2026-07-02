#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

// Formatter: declarative formatter via subprocess (e.g., clang-format, rustfmt).
struct FormatterSpec {
  std::string id;
  std::string language_id;
  std::string label;
  std::vector<std::string> command;  // e.g., ["clang-format"]
  std::string plugin_id;
};

using FormatterRegistry = ProviderRegistry<FormatterSpec>;

// First formatter for `language_id` (or nullptr).
inline const FormatterSpec* FindFormatter(const FormatterRegistry& registry,
                                          std::string_view language_id) {
  return registry.FindIf(
      [&](const FormatterSpec& spec) { return spec.language_id == language_id; });
}

}  // namespace microide::workspace
