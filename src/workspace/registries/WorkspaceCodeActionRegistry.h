#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

struct CodeActionProviderSpec {
  std::string id;
  std::string plugin_id;
  std::string language_id;
};

using CodeActionRegistry = ProviderRegistry<CodeActionProviderSpec>;

inline const CodeActionProviderSpec* FindProvider(const CodeActionRegistry& registry,
                                                  std::string_view language_id) {
  return registry.FindIf(
      [&](const CodeActionProviderSpec& spec) { return spec.language_id == language_id; });
}

}
