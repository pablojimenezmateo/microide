#include "workspace/WorkspaceCompletionRegistry.h"

namespace microide::workspace {

CompletionRegistry::CompletionRegistry() = default;
CompletionRegistry::~CompletionRegistry() = default;

void CompletionRegistry::Register(const CompletionProviderSpec& spec) {
  specs_.push_back(spec);
}

const CompletionProviderSpec* CompletionRegistry::FindProvider(
    const std::string& language_id) const {
  for (const auto& spec : specs_) {
    if (spec.language_id == language_id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
