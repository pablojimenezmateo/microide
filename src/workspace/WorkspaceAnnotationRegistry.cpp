#include "workspace/WorkspaceAnnotationRegistry.h"

namespace microide::workspace {

AnnotationRegistry::AnnotationRegistry() = default;
AnnotationRegistry::~AnnotationRegistry() = default;

void AnnotationRegistry::Register(const AnnotationProviderSpec& spec) {
  specs_.push_back(spec);
}

}  // namespace microide::workspace
