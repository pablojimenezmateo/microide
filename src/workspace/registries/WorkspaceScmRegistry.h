#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

struct ScmProviderSpec {
  std::string id;
  std::string label;
  std::string plugin_id;
};

using ScmRegistry = ProviderRegistry<ScmProviderSpec>;

inline const ScmProviderSpec* FindProvider(const ScmRegistry& registry, std::string_view id) {
  return registry.FindIf([&](const ScmProviderSpec& spec) { return spec.id == id; });
}

}
