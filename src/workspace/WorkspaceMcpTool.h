#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// MCP tool: Model Context Protocol tool that agents can use.
struct McpToolSpec {
  std::string id;
  std::string name;
  std::string description;
  std::string input_schema;  // JSON schema for tool input
  std::string plugin_id;
};

// Tool permission: controls which agents/contexts can use which tools.
enum class ToolPermissionLevel {
  Denied,
  PromptRequired,
  AllowedWithinContext,
  Allowed,
};

struct ToolPermission {
  std::string tool_id;
  std::string agent_id;  // "*" for all agents
  ToolPermissionLevel level = ToolPermissionLevel::PromptRequired;
};

// MCP tool registry: manages tools and permissions.
class McpToolRegistry {
 public:
  McpToolRegistry();
  ~McpToolRegistry();

  void RegisterTool(const McpToolSpec& spec);
  const std::vector<McpToolSpec>& Specs() const { return tools_; }

  // Find tool by id.
  const McpToolSpec* FindTool(const std::string& id) const;

  // Set permission for tool usage.
  void SetPermission(const ToolPermission& perm);

  // Check if agent can use tool.
  ToolPermissionLevel CheckPermission(const std::string& tool_id,
                                      const std::string& agent_id) const;

  // Get all tools available to an agent.
  std::vector<const McpToolSpec*> GetAvailableTools(const std::string& agent_id) const;

  // Clear all.
  void Clear();

 private:
  std::vector<McpToolSpec> tools_;
  std::vector<ToolPermission> permissions_;
};

}  // namespace microide::workspace
