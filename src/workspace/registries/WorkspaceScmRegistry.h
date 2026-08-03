#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

// SCM provider: source control system plugin (Git, Hg, Perforce, etc.)
struct ScmProviderSpec {
  std::string id;
  std::string label;
  std::string plugin_id;
};

using ScmRegistry = ProviderRegistry<ScmProviderSpec>;

// First SCM provider matching `id` (or nullptr).
inline const ScmProviderSpec* FindProvider(const ScmRegistry& registry, std::string_view id) {
  return registry.FindIf([&](const ScmProviderSpec& spec) { return spec.id == id; });
}

}  // namespace microide::workspace
