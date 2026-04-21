#include "workspace/WorkspaceMcpTool.h"

#include <algorithm>

namespace microide::workspace {

McpToolRegistry::McpToolRegistry() = default;
McpToolRegistry::~McpToolRegistry() = default;

void McpToolRegistry::RegisterTool(const McpToolSpec& spec) { tools_.push_back(spec); }

const McpToolSpec* McpToolRegistry::FindTool(const std::string& id) const {
  for (const auto& tool : tools_) {
    if (tool.id == id) {
      return &tool;
    }
  }
  return nullptr;
}

void McpToolRegistry::SetPermission(const ToolPermission& perm) {
  for (auto& p : permissions_) {
    if (p.tool_id == perm.tool_id && p.agent_id == perm.agent_id) {
      p.level = perm.level;
      return;
    }
  }
  permissions_.push_back(perm);
}

ToolPermissionLevel McpToolRegistry::CheckPermission(const std::string& tool_id,
                                                     const std::string& agent_id) const {
  // Check agent-specific permission first
  for (const auto& perm : permissions_) {
    if (perm.tool_id == tool_id && perm.agent_id == agent_id) {
      return perm.level;
    }
  }
  // Check global permission
  for (const auto& perm : permissions_) {
    if (perm.tool_id == tool_id && perm.agent_id == "*") {
      return perm.level;
    }
  }
  // Default to prompt required
  return ToolPermissionLevel::PromptRequired;
}

std::vector<const McpToolSpec*> McpToolRegistry::GetAvailableTools(
    const std::string& agent_id) const {
  std::vector<const McpToolSpec*> result;
  for (const auto& tool : tools_) {
    const auto perm = CheckPermission(tool.id, agent_id);
    if (perm != ToolPermissionLevel::Denied) {
      result.push_back(&tool);
    }
  }
  return result;
}

void McpToolRegistry::Clear() {
  tools_.clear();
  permissions_.clear();
}

}  // namespace microide::workspace
