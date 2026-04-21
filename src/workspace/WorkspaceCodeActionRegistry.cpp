#include "workspace/WorkspaceCodeActionRegistry.h"

namespace microide::workspace {

CodeActionRegistry::CodeActionRegistry() = default;
CodeActionRegistry::~CodeActionRegistry() = default;

void CodeActionRegistry::Register(const CodeActionProviderSpec& spec) {
  specs_.push_back(spec);
}

const CodeActionProviderSpec* CodeActionRegistry::FindProvider(
    const std::string& language_id) const {
  for (const auto& spec : specs_) {
    if (spec.language_id == language_id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
