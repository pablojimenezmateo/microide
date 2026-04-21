#include "workspace/WorkspaceExternalAgent.h"

#include <algorithm>

namespace microide::workspace {

ExternalAgentRegistry::ExternalAgentRegistry() = default;
ExternalAgentRegistry::~ExternalAgentRegistry() = default;

void ExternalAgentRegistry::RegisterAgent(const ExternalAgentSpec& spec) {
  agents_.push_back(spec);
}

const ExternalAgentSpec* ExternalAgentRegistry::FindAgent(const std::string& id) const {
  for (const auto& agent : agents_) {
    if (agent.id == id) {
      return &agent;
    }
  }
  return nullptr;
}

std::vector<const ExternalAgentSpec*> ExternalAgentRegistry::FindByCapability(
    const std::string& capability) const {
  std::vector<const ExternalAgentSpec*> result;
  for (const auto& agent : agents_) {
    for (const auto& cap : agent.capabilities) {
      if (cap == capability) {
        result.push_back(&agent);
        break;
      }
    }
  }
  return result;
}

void ExternalAgentRegistry::SetSelection(const std::string& task,
                                         const AgentSelection& selection) {
  for (auto& sel : selections_) {
    if (sel.task == task) {
      sel = selection;
      return;
    }
  }
  selections_.push_back(selection);
}

const AgentSelection* ExternalAgentRegistry::GetSelection(const std::string& task) const {
  for (const auto& selection : selections_) {
    if (selection.task == task) {
      return &selection;
    }
  }
  return nullptr;
}

void ExternalAgentRegistry::Clear() {
  agents_.clear();
  selections_.clear();
}

}  // namespace microide::workspace
