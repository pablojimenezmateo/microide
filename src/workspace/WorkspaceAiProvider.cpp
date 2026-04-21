#include "workspace/WorkspaceAiProvider.h"

namespace microide::workspace {

AiProviderRegistry::AiProviderRegistry() = default;
AiProviderRegistry::~AiProviderRegistry() = default;

void AiProviderRegistry::Register(const AiProviderSpec& spec) { specs_.push_back(spec); }

const AiProviderSpec* AiProviderRegistry::FindProvider(const std::string& id) const {
  for (const auto& spec : specs_) {
    if (spec.id == id) {
      return &spec;
    }
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::string>> AiProviderRegistry::AllModels() const {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& provider : specs_) {
    for (const auto& model : provider.models) {
      result.push_back({provider.id, model});
    }
  }
  return result;
}

}  // namespace microide::workspace
