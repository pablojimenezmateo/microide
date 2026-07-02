#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

// Completion provider: Lua function that returns completion items.
struct CompletionProviderSpec {
  std::string id;
  std::string plugin_id;
  std::string language_id;
  std::string trigger_characters;  // e.g., "." or "->."
};

using CompletionRegistry = ProviderRegistry<CompletionProviderSpec>;

// First completion provider for `language_id` (or nullptr).
inline const CompletionProviderSpec* FindProvider(const CompletionRegistry& registry,
                                                  std::string_view language_id) {
  return registry.FindIf(
      [&](const CompletionProviderSpec& spec) { return spec.language_id == language_id; });
}

}  // namespace microide::workspace
