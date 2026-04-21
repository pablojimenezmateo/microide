#include "workspace/WorkspaceTaskRegistry.h"

namespace microide::workspace {

TaskRegistry::TaskRegistry() = default;
TaskRegistry::~TaskRegistry() = default;

void TaskRegistry::Register(const TaskSpec& spec) { specs_.push_back(spec); }

const TaskSpec* TaskRegistry::FindTask(const std::string& id) const {
  for (const auto& spec : specs_) {
    if (spec.id == id) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace microide::workspace
