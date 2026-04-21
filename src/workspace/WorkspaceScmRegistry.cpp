#include "workspace/WorkspaceScmRegistry.h"

namespace microide::workspace {

ScmRegistry::ScmRegistry() = default;
ScmRegistry::~ScmRegistry() = default;

void ScmRegistry::Register(const ScmProviderSpec& spec) { specs_.push_back(spec); }

const ScmProviderSpec* ScmRegistry::FindProvider(const std::string& id) const {
  for (const auto& spec : specs_) {
    if (spec.id == id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
