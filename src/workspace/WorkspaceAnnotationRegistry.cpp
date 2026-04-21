#include "workspace/WorkspaceAnnotationRegistry.h"

namespace microide::workspace {

AnnotationRegistry::AnnotationRegistry() = default;
AnnotationRegistry::~AnnotationRegistry() = default;

void AnnotationRegistry::Register(const AnnotationProviderSpec& spec) {
  specs_.push_back(spec);
}

std::vector<const AnnotationProviderSpec*> AnnotationRegistry::FindProviders(
    const std::string& language_id) const {
  std::vector<const AnnotationProviderSpec*> result;
  for (const auto& spec : specs_) {
    if (spec.language_id == language_id) {
      result.push_back(&spec);
    }
  }
  return result;
}

std::vector<const AnnotationProviderSpec*> AnnotationRegistry::FindByType(
    const std::string& type) const {
  std::vector<const AnnotationProviderSpec*> result;
  for (const auto& spec : specs_) {
    if (spec.type == type) {
      result.push_back(&spec);
    }
  }
  return result;
}

}  // namespace microide::workspace
