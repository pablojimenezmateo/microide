#include "workspace/WorkspaceAiProvider.h"

#include <utility>

namespace microide::workspace {

AiProviderRegistry::AiProviderRegistry() = default;
AiProviderRegistry::~AiProviderRegistry() = default;

void AiProviderRegistry::Register(const AiProviderSpec& spec) {
  if (spec.id.empty()) {
    return;
  }
  AiProviderSpec normalized = spec;
  if (normalized.display_name.empty()) {
    normalized.display_name = normalized.label;
  }
  if (normalized.display_name.empty()) {
    return;
  }
  if (normalized.auth_method.empty()) {
    normalized.auth_method = normalized.requires_api_key ? "api_key" : "none";
  }
  if (!normalized.api_key_name.empty()) {
    normalized.requires_api_key = true;
    if (normalized.auth_method == "none") {
      normalized.auth_method = "api_key";
    }
  }
  specs_.push_back(std::move(normalized));
}

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
