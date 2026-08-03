#pragma once

#include <string>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

// Blame/annotation provider: adds inline blame, decoration, or margin annotations.
struct AnnotationProviderSpec {
  std::string id;
  std::string label;
  std::string type;  // "blame", "decoration", "margin"
  std::string language_id;
  std::string plugin_id;
};

using AnnotationRegistry = ProviderRegistry<AnnotationProviderSpec>;

}  // namespace microide::workspace
