#include "workspace/WorkspaceFormatterRegistry.h"

namespace microide::workspace {

FormatterRegistry::FormatterRegistry() = default;
FormatterRegistry::~FormatterRegistry() = default;

void FormatterRegistry::Register(const FormatterSpec& spec) { specs_.push_back(spec); }

const FormatterSpec* FormatterRegistry::FindFormatter(const std::string& language_id) const {
  for (const auto& spec : specs_) {
    if (spec.language_id == language_id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
