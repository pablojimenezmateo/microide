#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// External agent: AI agent accessed over a protocol (ACP, stdio, HTTP, etc.)
struct ExternalAgentSpec {
  std::string id;
  std::string label;
  std::string protocol;  // "acp", "stdio", "http", "websocket"
  std::vector<std::string> command;
  std::vector<std::string> capabilities;  // "inline-completion", "chat", "explain", etc.
  std::string plugin_id;
};

// Agent selection: which agent(s) to use for a given task.
struct AgentSelection {
  std::string task;  // "completion", "chat", "explain", "fix"
  std::string preferred_agent;
  std::vector<std::string> fallback_agents;
};

// External agent registry: manages external AI agents and selection policies.
class ExternalAgentRegistry {
 public:
  ExternalAgentRegistry();
  ~ExternalAgentRegistry();

  void RegisterAgent(const ExternalAgentSpec& spec);
  const std::vector<ExternalAgentSpec>& Specs() const { return agents_; }

  // Find agent by id.
  const ExternalAgentSpec* FindAgent(const std::string& id) const;

  // Get agents that support a capability.
  std::vector<const ExternalAgentSpec*> FindByCapability(const std::string& capability) const;

  // Set agent selection for a task.
  void SetSelection(const std::string& task, const AgentSelection& selection);

  // Get selected agent for a task.
  const AgentSelection* GetSelection(const std::string& task) const;

  // Clear all.
  void Clear();

 private:
  std::vector<ExternalAgentSpec> agents_;
  std::vector<AgentSelection> selections_;
};

}  // namespace microide::workspace
