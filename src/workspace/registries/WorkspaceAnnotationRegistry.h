#pragma once

#include <string>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

struct AnnotationProviderSpec {
  std::string id;
  std::string label;
  std::string type;
  std::string language_id;
  std::string plugin_id;
};

using AnnotationRegistry = ProviderRegistry<AnnotationProviderSpec>;

}
