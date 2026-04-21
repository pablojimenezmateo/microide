#include "workspace/WorkspaceToolRegistry.h"

namespace microide::workspace {

ToolRegistry::ToolRegistry() = default;
ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::Register(const ToolSpec& spec) { specs_.push_back(spec); }

const ToolSpec* ToolRegistry::FindTool(const std::string& id, const std::string& platform) const {
  for (const auto& spec : specs_) {
    if (spec.id == id && spec.platform == platform) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
