#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

// Code action provider: Lua function that returns code actions for a range.
struct CodeActionProviderSpec {
  std::string id;
  std::string plugin_id;
  std::string language_id;
};

using CodeActionRegistry = ProviderRegistry<CodeActionProviderSpec>;

// First code-action provider for `language_id` (or nullptr).
inline const CodeActionProviderSpec* FindProvider(const CodeActionRegistry& registry,
                                                  std::string_view language_id) {
  return registry.FindIf(
      [&](const CodeActionProviderSpec& spec) { return spec.language_id == language_id; });
}

}  // namespace microide::workspace
