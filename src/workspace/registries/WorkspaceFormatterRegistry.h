#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

struct FormatterSpec {
  std::string id;
  std::string language_id;
  std::string label;
  std::vector<std::string> command;
  std::string plugin_id;
};

using FormatterRegistry = ProviderRegistry<FormatterSpec>;

inline const FormatterSpec* FindFormatter(const FormatterRegistry& registry,
                                          std::string_view language_id) {
  return registry.FindIf(
      [&](const FormatterSpec& spec) { return spec.language_id == language_id; });
}

}
