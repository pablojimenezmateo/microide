#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

#include "util/JsonValue.h"
#include "util/SingleLineText.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kToolApprovalTimeoutMs = 30000;

std::string GenerateRuntimeMessageId(std::string_view prefix) {
  static std::uint64_t counter = 1;
  return std::string(prefix) + "-" + std::to_string(counter++);
}

std::string CurrentUtcTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    return {};
  }
  return buffer;
}

void LoadChatComposerViewport(editor::TextViewport* viewport, std::string_view text) {
  if (viewport == nullptr) {
    return;
  }
  viewport->LoadContent(text, {});
  viewport->SetDirty(false);
  viewport->SetViewportSize(4, 40);
  const std::size_t last_line = viewport->line_count() > 0 ? viewport->line_count() - 1 : 0;
  viewport->MoveCursorTo(last_line, viewport->lines().empty() ? 0 : viewport->lines().back().size());
}

std::string NextConversationTitle(const ConversationRegistry& registry) {
  const std::size_t count = registry.conversations().size();
  return count == 0 ? "Chat" : "Chat " + std::to_string(count + 1);
}

std::string SerializeToolMode(ToolMode mode) {
  switch (mode) {
    case ToolMode::NoTools: return "no_tools";
    case ToolMode::Ask: return "ask";
    case ToolMode::Auto: return "auto";
  }
  return "ask";
}

std::string CollapseWhitespaceForSummary(std::string_view text) {
  std::string out;
  bool in_space = false;
  for (char ch : text) {
    const bool whitespace = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (whitespace) {
      if (!in_space && !out.empty()) {
        out.push_back(' ');
      }
      in_space = true;
      continue;
    }
    in_space = false;
    out.push_back(ch);
  }
  return out;
}

std::string TruncateSummary(std::string text, std::size_t max_length = 160) {
  if (text.size() <= max_length) {
    return text;
  }
  text.resize(max_length - 3);
  text += "...";
  return text;
}

std::string ToolOutputSummary(std::string_view output_json) {
  return TruncateSummary(CollapseWhitespaceForSummary(output_json));
}

std::string JoinSummaryParts(const std::vector<std::string>& parts) {
  std::string result;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!result.empty()) {
      result += " · ";
    }
    result += part;
  }
  return result;
}

std::string ContextPolicySummary(const ContextPolicy& policy,
                                 const std::vector<ContextItem>& items) {
  std::vector<std::string> parts;
  parts.push_back(std::to_string(items.size()) + " item" + (items.size() == 1 ? "" : "s"));
  parts.push_back(std::to_string(policy.max_total_bytes / 1024) + "KB max");
  parts.push_back(std::to_string(policy.max_files) + " files");
  if (policy.include_diagnostics) {
    parts.push_back("diagnostics");
  }
  if (policy.include_git_context) {
    parts.push_back("git");
  }
  if (policy.include_project_structure) {
    parts.push_back("structure");
  }
  return JoinSummaryParts(parts);
}

const ExternalAgentSpec* SelectAgentForCapability(const ExternalAgentRegistry& registry,
                                                  const std::string& capability) {
  if (const AgentSelection* selection = registry.GetSelection(capability);
      selection != nullptr && !selection->preferred_agent.empty()) {
    if (const auto* agent = registry.FindAgent(selection->preferred_agent); agent != nullptr) {
      return agent;
    }
  }
  const auto agents = registry.FindByCapability(capability);
  return agents.empty() ? nullptr : agents.front();
}

bool AgentSupportsCapability(const ExternalAgentSpec* agent, std::string_view capability) {
  if (agent == nullptr) {
    return false;
  }
  return std::find(agent->capabilities.begin(), agent->capabilities.end(), capability) !=
         agent->capabilities.end();
}

void UpdateMessageContent(Conversation* conversation,
                          std::string_view message_id,
                          std::string content) {
  if (conversation == nullptr) {
    return;
  }
  const auto build_render_line = [](MessageRole role, std::string_view raw_content) {
    const std::string_view prefix =
        role == MessageRole::Assistant ? std::string_view{"Assistant"}
        : role == MessageRole::User   ? std::string_view{"You"}
                                       : std::string_view{"System"};
    const std::string collapsed = CollapseWhitespace(raw_content);
    std::string line;
    line.reserve(prefix.size() + 2 + collapsed.size());
    line += prefix;
    line += ": ";
    line += collapsed;
    return line;
  };
  for (auto& message : conversation->messages) {
    if (message.id == message_id) {
      message.content = std::move(content);
      message.render_line = build_render_line(message.role, message.content);
      message.timestamp = CurrentUtcTimestamp();
      return;
    }
  }
}

Message* FindMessage(Conversation* conversation, std::string_view message_id) {
  if (conversation == nullptr) {
    return nullptr;
  }
  for (auto& message : conversation->messages) {
    if (message.id == message_id) {
      return &message;
    }
  }
  return nullptr;
}

ToolEvent* FindToolEvent(Message* message, std::string_view call_id) {
  if (message == nullptr) {
    return nullptr;
  }
  for (auto& event : message->tool_events) {
    if (event.call_id == call_id) {
      return &event;
    }
  }
  return nullptr;
}

RequestStatus ParseBridgeTerminalStatus(std::string_view status_text) {
  if (status_text == "succeeded") {
    return RequestStatus::Succeeded;
  }
  if (status_text == "cancelled") {
    return RequestStatus::Cancelled;
  }
  return RequestStatus::Failed;
}

std::string RequestStatusText(RequestStatus status) {
  switch (status) {
    case RequestStatus::Idle: return "Idle";
    case RequestStatus::Queued: return "Queued";
    case RequestStatus::Running: return "Running";
    case RequestStatus::Streaming: return "Streaming";
    case RequestStatus::Succeeded: return "Succeeded";
    case RequestStatus::Failed: return "Failed";
    case RequestStatus::Cancelled: return "Cancelled";
  }
  return "Idle";
}

ProjectWorkspaceState* FindProjectForChatRequest(WorkspaceContext& context,
                                                 std::string_view agent_id,
                                                 std::string_view request_id,
                                                 bool* active_project) {
  auto matches = [&](ProjectWorkspaceState& project) {
    return project.panel.chat.request_in_flight &&
           project.panel.chat.pending_bridge_agent_id == agent_id &&
           project.panel.chat.pending_bridge_request_id == request_id;
  };

  if (matches(context.current_project_state)) {
    if (active_project != nullptr) {
      *active_project = true;
    }
    return &context.current_project_state;
  }
  for (std::size_t i = 0; i < context.project_catalog.entries.size(); ++i) {
    ProjectWorkspaceState* project = context.project_catalog.entries[i].get();
    if (project != nullptr && matches(*project)) {
      if (active_project != nullptr) {
        *active_project = false;
      }
      return project;
    }
  }
  return nullptr;
}

const char* ToolPermissionDecisionLabel(ToolPermissionLevel permission) {
  switch (permission) {
    case ToolPermissionLevel::Denied:
      return "denied";
    case ToolPermissionLevel::PromptRequired:
      return "prompt";
    case ToolPermissionLevel::AllowedWithinContext:
      return "context";
    case ToolPermissionLevel::Allowed:
      return "auto";
  }
  return "prompt";
}

}  // namespace

std::size_t WorkspaceShell::CountOpenBufferViews(const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return 0;
  }

  std::size_t count = 0;
  for (const auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        if (EditorViewPath(view) == normalized_path) {
          ++count;
        }
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      if (tab.compare->right_editable &&
          tab.compare->right_viewport.path().lexically_normal() == normalized_path) {
        ++count;
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.path().lexically_normal() == normalized_path) {
      ++count;
    }
  }
  return count;
}

bool WorkspaceShell::CancelActiveChatRequest() {
  ChatPanelState& chat = context_.current_project_state.panel.chat;
  Conversation* conversation =
      context_.current_project_state.conversations.GetConversation(chat.request_conversation_id);
  if (conversation == nullptr || !chat.request_in_flight || chat.pending_assistant_message_id.empty()) {
    return false;
  }

  provider_bridge_manager_.CancelRequest(chat.pending_bridge_agent_id, chat.pending_bridge_request_id);
  const std::int64_t duration_ms =
      chat.request_started_ticks == 0
          ? 0
          : static_cast<std::int64_t>(SDL_GetTicks() - chat.request_started_ticks);
  const std::string cancelled_call_id =
      chat.pending_tool_approval.has_value() ? chat.pending_tool_approval->tool_call_id : std::string{};
  conversation->status = RequestStatus::Cancelled;
  conversation->last_request_duration_ms = duration_ms;
  for (auto& message : conversation->messages) {
    if (message.id == chat.pending_assistant_message_id) {
      message.status = RequestStatus::Cancelled;
      message.request_duration_ms = duration_ms;
      if (message.error.empty()) {
        message.error = "Cancelled";
      }
      if (!cancelled_call_id.empty()) {
        if (ToolEvent* event = FindToolEvent(&message, cancelled_call_id); event != nullptr) {
          event->status = "Cancelled";
          event->permission_decision = "cancelled";
          event->finished_at = CurrentUtcTimestamp();
          event->duration_ms = duration_ms;
          if (event->error.empty()) {
            event->error = "Cancelled";
          }
        }
      }
      break;
    }
  }

  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::ApproveChatTool &&
      context_.prompts.surface.bridge_agent_id == chat.pending_bridge_agent_id &&
      context_.prompts.surface.bridge_request_id == chat.pending_bridge_request_id) {
    DismissPromptSurface(false);
  }
  chat.pending_tool_approval.reset();
  chat.request_in_flight = false;
  chat.request_started_ticks = 0;
  chat.status_text = "Cancelled";
  chat.request_conversation_id.clear();
  chat.pending_assistant_message_id.clear();
  chat.pending_bridge_agent_id.clear();
  chat.pending_bridge_request_id.clear();
  chat.active_request = ChatPanelState::RequestSnapshot{};
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::StartChatRequest(std::string message, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (message.empty()) {
    message = ChatComposerText();
  }
  if (message.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat message is empty";
    }
    return false;
  }

  ShowChatPanel();
  if (context_.current_project_state.panel.chat.request_in_flight) {
    if (error_message != nullptr) {
      *error_message = "Another chat request is already running in this project";
    }
    return false;
  }
  Conversation* conversation =
      ActiveConversation();
  if (conversation == nullptr) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
    conversation = ActiveConversation();
  }

  const ExternalAgentSpec* agent = nullptr;
  if (conversation != nullptr && !conversation->provider_id.empty()) {
    const ExternalAgentSpec* preferred =
        external_agent_registry_.FindAgent(conversation->provider_id);
    if (AgentSupportsCapability(preferred, "chat")) {
      agent = preferred;
    }
  }
  if (agent == nullptr) {
    agent = SelectAgentForCapability(external_agent_registry_, "chat");
  }
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio chat agent is available";
    }
    return false;
  }

  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
    if (auth_status == ProviderAuthStatus::KeyMissing ||
        auth_status == ProviderAuthStatus::KeyInvalid) {
      if (error_message != nullptr) {
        *error_message = ChatAuthBannerText(conversation);
      }
      return false;
    }
  }

  const std::string user_id = GenerateRuntimeMessageId("user");
  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  conversation->provider_id = agent->id;
  conversation->draft.clear();
  {
    Message user_msg;
    user_msg.id = user_id;
    user_msg.role = MessageRole::User;
    user_msg.content = message;
    user_msg.timestamp = CurrentUtcTimestamp();
    user_msg.status = RequestStatus::Succeeded;
    context_.current_project_state.conversations.AddMessage(conversation->id, user_msg);
  }
  {
    Message assistant_msg;
    assistant_msg.id = assistant_id;
    assistant_msg.role = MessageRole::Assistant;
    assistant_msg.timestamp = CurrentUtcTimestamp();
    assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : agent->id;
    assistant_msg.status = RequestStatus::Running;
    context_.current_project_state.conversations.AddMessage(conversation->id, assistant_msg);
  }
  conversation->status = RequestStatus::Running;
  conversation->last_request_duration_ms = 0;
  context_.current_project_state.panel.chat.request_conversation_id = conversation->id;
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.request_started_ticks = SDL_GetTicks();
  context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{
      .provider_id = agent->id,
      .model_id = conversation != nullptr ? conversation->model_id : std::string{},
      .tool_mode = conversation != nullptr ? conversation->tool_mode : ToolMode::NoTools,
      .context_policy = ai_context_manager_.policy(),
      .context_items = ai_context_manager_.GetContext(),
  };
  context_.current_project_state.panel.chat.pending_tool_approval.reset();
  context_.current_project_state.panel.chat.status_text = "Waiting for " + agent->label;
  LoadChatComposerViewport(&context_.current_project_state.panel.chat.composer, {});
  const auto mark_failed = [&](std::string_view reason) {
    conversation->status = RequestStatus::Failed;
    for (auto& item : conversation->messages) {
      if (item.id == assistant_id) {
        item.status = RequestStatus::Failed;
        item.error = std::string(reason);
        break;
      }
    }
    context_.current_project_state.panel.chat.status_text = std::string(reason);
  };

  if (agent->command.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat agent command is empty";
    }
    mark_failed("Chat agent command is empty");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::string api_key;
  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    api_key = ResolveProviderApiKey(*provider).value_or("");
  }
  if (!provider_bridge_manager_.IsBridgeRunning(agent->id) &&
      !provider_bridge_manager_.StartBridge(agent->id,
                                            agent->command,
                                            api_key,
                                            context_.current_project_state.root)) {
    if (error_message != nullptr) {
      *error_message = "Failed to start chat agent bridge";
    }
    mark_failed("Failed to start chat agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::vector<std::pair<std::string, std::string>> bridge_messages;
  if (conversation != nullptr) {
    for (const auto& msg : conversation->messages) {
      if (msg.role == MessageRole::User && msg.id != user_id) {
        bridge_messages.emplace_back("user", msg.content);
      } else if (msg.role == MessageRole::Assistant &&
                 msg.status == RequestStatus::Succeeded) {
        bridge_messages.emplace_back("assistant", msg.content);
      } else if (msg.id == user_id) {
        bridge_messages.emplace_back("user", msg.content);
        break;
      }
    }
  }
  if (bridge_messages.empty() || bridge_messages.back().first != "user") {
    bridge_messages.emplace_back("user", message);
  }

  const std::string request_id = GenerateRuntimeMessageId("bridge-req");
  const std::string model_id = conversation != nullptr ? conversation->model_id : "";
  const std::string system_prompt = conversation != nullptr ? conversation->system_prompt : "";
  const ToolMode tool_mode = conversation != nullptr ? conversation->tool_mode : ToolMode::NoTools;
  const std::string tool_mode_str = SerializeToolMode(tool_mode);
  std::vector<WorkspaceProviderBridgeManager::ToolSpec> bridge_tools;
  if (tool_mode != ToolMode::NoTools) {
    for (const McpToolSpec* tool : mcp_tool_registry_.GetAvailableTools(agent->id)) {
      if (tool == nullptr) {
        continue;
      }
      bridge_tools.push_back(WorkspaceProviderBridgeManager::ToolSpec{
          .id = tool->id,
          .display_name = !tool->name.empty() ? tool->name : tool->id,
          .description = tool->description,
          .input_schema = tool->input_schema,
      });
    }
  }

  context_.current_project_state.panel.chat.pending_bridge_agent_id = agent->id;
  context_.current_project_state.panel.chat.pending_bridge_request_id = request_id;
  if (!provider_bridge_manager_.SendChat(agent->id,
                                         request_id,
                                         bridge_messages,
                                         model_id,
                                         system_prompt,
                                         tool_mode_str,
                                         bridge_tools)) {
    if (error_message != nullptr) {
      *error_message = "Failed to send chat request to agent bridge";
    }
    mark_failed("Failed to send chat request to agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_agent_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_request_id.clear();
    context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{};
    return false;
  }

  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::RetryActiveChatRequest(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr || context_.current_project_state.panel.chat.request_in_flight) {
    return false;
  }

  auto last_user_it =
      std::find_if(conversation->messages.rbegin(), conversation->messages.rend(),
                   [](const Message& message) { return message.role == MessageRole::User; });
  if (last_user_it == conversation->messages.rend()) {
    if (error_message != nullptr) {
      *error_message = "No earlier user turn is available to retry";
    }
    return false;
  }
  const std::string message = last_user_it->content;

  const ExternalAgentSpec* agent = nullptr;
  if (!conversation->provider_id.empty()) {
    const ExternalAgentSpec* preferred =
        external_agent_registry_.FindAgent(conversation->provider_id);
    if (AgentSupportsCapability(preferred, "chat")) {
      agent = preferred;
    }
  }
  if (agent == nullptr) {
    agent = SelectAgentForCapability(external_agent_registry_, "chat");
  }
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio chat agent is available";
    }
    return false;
  }

  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
    if (auth_status == ProviderAuthStatus::KeyMissing ||
        auth_status == ProviderAuthStatus::KeyInvalid) {
      if (error_message != nullptr) {
        *error_message = ChatAuthBannerText(conversation);
      }
      return false;
    }
  }

  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  Message assistant_msg;
  assistant_msg.id = assistant_id;
  assistant_msg.role = MessageRole::Assistant;
  assistant_msg.timestamp = CurrentUtcTimestamp();
  assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : agent->id;
  assistant_msg.status = RequestStatus::Running;
  context_.current_project_state.conversations.AddMessage(conversation->id, assistant_msg);

  conversation->status = RequestStatus::Running;
  conversation->last_request_duration_ms = 0;
  context_.current_project_state.panel.chat.request_conversation_id = conversation->id;
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.request_started_ticks = SDL_GetTicks();
  context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{
      .provider_id = agent->id,
      .model_id = conversation->model_id,
      .tool_mode = conversation->tool_mode,
      .context_policy = ai_context_manager_.policy(),
      .context_items = ai_context_manager_.GetContext(),
  };
  context_.current_project_state.panel.chat.pending_tool_approval.reset();
  context_.current_project_state.panel.chat.status_text = "Retrying with " + agent->label;
  const auto mark_failed = [&](std::string_view reason) {
    conversation->status = RequestStatus::Failed;
    for (auto& item : conversation->messages) {
      if (item.id == assistant_id) {
        item.status = RequestStatus::Failed;
        item.error = std::string(reason);
        break;
      }
    }
    context_.current_project_state.panel.chat.status_text = std::string(reason);
  };

  std::string api_key;
  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    api_key = ResolveProviderApiKey(*provider).value_or("");
  }
  if (!provider_bridge_manager_.IsBridgeRunning(agent->id) &&
      !provider_bridge_manager_.StartBridge(agent->id, agent->command, api_key,
                                            context_.current_project_state.root)) {
    if (error_message != nullptr) {
      *error_message = "Failed to start chat agent bridge";
    }
    mark_failed("Failed to start chat agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::vector<std::pair<std::string, std::string>> bridge_messages;
  for (const auto& msg : conversation->messages) {
    if (msg.role == MessageRole::User) {
      bridge_messages.emplace_back("user", msg.content);
      if (msg.id == last_user_it->id) {
        break;
      }
    } else if (msg.role == MessageRole::Assistant &&
               msg.status == RequestStatus::Succeeded) {
      bridge_messages.emplace_back("assistant", msg.content);
    }
  }
  if (bridge_messages.empty() || bridge_messages.back().first != "user") {
    bridge_messages.emplace_back("user", message);
  }

  const std::string request_id = GenerateRuntimeMessageId("bridge-req");
  const std::string tool_mode_str = SerializeToolMode(conversation->tool_mode);
  std::vector<WorkspaceProviderBridgeManager::ToolSpec> bridge_tools;
  if (conversation->tool_mode != ToolMode::NoTools) {
    for (const McpToolSpec* tool : mcp_tool_registry_.GetAvailableTools(agent->id)) {
      if (tool == nullptr) {
        continue;
      }
      bridge_tools.push_back(WorkspaceProviderBridgeManager::ToolSpec{
          .id = tool->id,
          .display_name = !tool->name.empty() ? tool->name : tool->id,
          .description = tool->description,
          .input_schema = tool->input_schema,
      });
    }
  }
  context_.current_project_state.panel.chat.pending_bridge_agent_id = agent->id;
  context_.current_project_state.panel.chat.pending_bridge_request_id = request_id;
  if (!provider_bridge_manager_.SendChat(agent->id, request_id, bridge_messages,
                                         conversation->model_id, conversation->system_prompt,
                                         tool_mode_str, bridge_tools)) {
    if (error_message != nullptr) {
      *error_message = "Failed to send chat request to agent bridge";
    }
    mark_failed("Failed to send chat request to agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_agent_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_request_id.clear();
    context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{};
    return false;
  }

  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::ShowPendingToolApprovalPrompt(ProjectWorkspaceState& project) {
  if (!project.panel.chat.pending_tool_approval.has_value()) {
    return;
  }
  const auto& pending = *project.panel.chat.pending_tool_approval;
  const std::string args_detail =
      pending.arguments_summary.empty() ? std::string("No arguments")
                                        : pending.arguments_summary;
  const std::string context_detail =
      ContextPolicySummary(project.panel.chat.active_request.context_policy,
                           project.panel.chat.active_request.context_items);

  RequestPromptRedraw();
  context_.prompts.surface_visible = true;
  context_.prompts.surface_previous_focus = project.surface.focus;
  context_.prompts.surface.kind = PromptSurfaceState::Kind::Confirm;
  context_.prompts.surface.action = PromptSurfaceState::Action::ApproveChatTool;
  context_.prompts.surface.path = project.root;
  util::SetSingleLineText(&context_.prompts.surface.input, {});
  context_.prompts.surface.detail =
      JoinSummaryParts({pending.capability_scope.empty() ? pending.tool_id : pending.capability_scope,
                        "Args: " + args_detail, context_detail});
  context_.prompts.surface.bridge_agent_id = pending.bridge_agent_id;
  context_.prompts.surface.bridge_request_id = pending.bridge_request_id;
  context_.prompts.surface.tool_call_id = pending.tool_call_id;
  context_.prompts.surface.tool_id = pending.display_name.empty() ? pending.tool_id
                                                                  : pending.display_name;
  context_.prompts.surface.capability_scope = pending.capability_scope;
  context_.prompts.surface.button_count = 3;
  context_.prompts.surface.selected_button = 0;
  project.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

bool WorkspaceShell::ResolveChatToolApprovalPrompt(bool allow, bool remember_for_session) {
  if (!context_.prompts.surface_visible ||
      context_.prompts.surface.action != PromptSurfaceState::Action::ApproveChatTool) {
    return false;
  }

  ProjectWorkspaceState* project = nullptr;
  if (context_.current_project_state.root == context_.prompts.surface.path) {
    project = &context_.current_project_state;
  } else {
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr && entry->root == context_.prompts.surface.path) {
        project = entry.get();
        break;
      }
    }
  }
  if (project == nullptr || !project->panel.chat.pending_tool_approval.has_value()) {
    DismissPromptSurface(true);
    return false;
  }

  ChatPanelState& chat = project->panel.chat;
  const auto pending = *chat.pending_tool_approval;
  if (pending.bridge_agent_id != context_.prompts.surface.bridge_agent_id ||
      pending.bridge_request_id != context_.prompts.surface.bridge_request_id ||
      pending.tool_call_id != context_.prompts.surface.tool_call_id) {
    DismissPromptSurface(true);
    return false;
  }

  Conversation* conversation = project->conversations.GetConversation(pending.conversation_id);
  Message* assistant = FindMessage(conversation, pending.assistant_message_id);
  ToolEvent* tool_event = FindToolEvent(assistant, pending.tool_call_id);
  const Uint64 now = SDL_GetTicks();

  if (!allow) {
    provider_bridge_manager_.SendToolDenied(
        pending.bridge_agent_id, pending.bridge_request_id, pending.tool_call_id,
        "Tool approval denied");
    if (tool_event != nullptr) {
      tool_event->status = "Denied";
      tool_event->permission_decision = "denied";
      tool_event->finished_at = CurrentUtcTimestamp();
      tool_event->duration_ms = pending.requested_ticks == 0
                                    ? 0
                                    : static_cast<std::int64_t>(now - pending.requested_ticks);
      tool_event->error = "Tool approval denied";
    }
    chat.status_text = "Tool approval denied";
  } else {
    if (remember_for_session) {
      const auto it = std::find_if(
          chat.remembered_tool_approvals.begin(), chat.remembered_tool_approvals.end(),
          [&](const ChatPanelState::RememberedToolApproval& approval) {
            return approval.capability_scope == pending.capability_scope;
          });
      if (it == chat.remembered_tool_approvals.end()) {
        chat.remembered_tool_approvals.push_back(ChatPanelState::RememberedToolApproval{
            .capability_scope = pending.capability_scope,
            .tool_id = pending.tool_id,
            .display_name = pending.display_name,
            .granted_at_ticks = now,
        });
      }
    }

    if (tool_event != nullptr) {
      tool_event->status = "Running";
      tool_event->permission_decision = remember_for_session ? "session" : "approved";
    }

    std::string output_json;
    std::string error_message;
    const bool succeeded = plugin_runtime_.Host().InvokeMcpTool(
        pending.tool_id, pending.arguments_json, &output_json, &error_message);
    if (succeeded) {
      provider_bridge_manager_.SendToolResult(
          pending.bridge_agent_id, pending.bridge_request_id, pending.tool_call_id, output_json);
      if (tool_event != nullptr) {
        tool_event->status = "Completed";
        tool_event->finished_at = CurrentUtcTimestamp();
        tool_event->duration_ms = pending.requested_ticks == 0
                                      ? 0
                                      : static_cast<std::int64_t>(now - pending.requested_ticks);
        tool_event->output_summary = ToolOutputSummary(output_json);
      }
      chat.status_text = "Tool completed";
    } else {
      const std::string denied_reason =
          error_message.empty() ? std::string("Tool execution failed") : error_message;
      provider_bridge_manager_.SendToolDenied(
          pending.bridge_agent_id, pending.bridge_request_id, pending.tool_call_id, denied_reason);
      if (tool_event != nullptr) {
        tool_event->status = "Failed";
        tool_event->finished_at = CurrentUtcTimestamp();
        tool_event->duration_ms = pending.requested_ticks == 0
                                      ? 0
                                      : static_cast<std::int64_t>(now - pending.requested_ticks);
        tool_event->error = denied_reason;
      }
      chat.status_text = denied_reason;
    }
  }

  chat.pending_tool_approval.reset();
  DismissPromptSurface(true);
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::ExpirePendingToolApprovals() {
  const Uint64 now = SDL_GetTicks();
  auto expire_project = [&](ProjectWorkspaceState& project) {
    ChatPanelState& chat = project.panel.chat;
    if (!chat.pending_tool_approval.has_value() ||
        chat.pending_tool_approval->expires_at_ticks == 0 ||
        now < chat.pending_tool_approval->expires_at_ticks) {
      return;
    }

    const auto pending = *chat.pending_tool_approval;
    Conversation* conversation = project.conversations.GetConversation(pending.conversation_id);
    Message* assistant = FindMessage(conversation, pending.assistant_message_id);
    if (ToolEvent* event = FindToolEvent(assistant, pending.tool_call_id); event != nullptr) {
      event->status = "Denied";
      event->permission_decision = "expired";
      event->finished_at = CurrentUtcTimestamp();
      event->duration_ms = pending.requested_ticks == 0
                               ? 0
                               : static_cast<std::int64_t>(now - pending.requested_ticks);
      event->error = "Tool approval timed out";
    }
    provider_bridge_manager_.SendToolDenied(
        pending.bridge_agent_id, pending.bridge_request_id, pending.tool_call_id,
        "Tool approval timed out");
    chat.pending_tool_approval.reset();
    if (context_.prompts.surface_visible &&
        context_.prompts.surface.action == PromptSurfaceState::Action::ApproveChatTool &&
        context_.prompts.surface.bridge_agent_id == pending.bridge_agent_id &&
        context_.prompts.surface.bridge_request_id == pending.bridge_request_id &&
        context_.prompts.surface.tool_call_id == pending.tool_call_id) {
      DismissPromptSurface(true);
    }
    chat.status_text = "Tool approval timed out";
  };

  expire_project(context_.current_project_state);
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry != nullptr) {
      expire_project(*entry);
    }
  }
}

void WorkspaceShell::ConsumeProviderBridgeUpdates() {
  ExpirePendingToolApprovals();
  while (true) {
    const auto update = provider_bridge_manager_.ConsumeChatUpdate();
    if (!update.has_value()) {
      break;
    }
    bool handled = false;

    bool active_project = false;
    if (ProjectWorkspaceState* chat_project =
            FindProjectForChatRequest(context_, update->agent_id, update->request_id,
                                      &active_project);
        chat_project != nullptr) {
      ChatPanelState& chat = chat_project->panel.chat;
      Conversation* conversation =
          chat_project->conversations.GetConversation(chat.request_conversation_id);
      Message* assistant = FindMessage(conversation, chat.pending_assistant_message_id);

      if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Chunk) {
        if (assistant != nullptr) {
          UpdateMessageContent(conversation, chat.pending_assistant_message_id, update->chunk);
        }
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        RequestChromeRedraw();
        handled = true;
      } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::ToolCall) {
        if (assistant != nullptr) {
          const std::string arguments_summary =
              !update->arguments_summary.empty()
                  ? update->arguments_summary
                  : TruncateSummary(CollapseWhitespaceForSummary(update->arguments_json));
          ToolEvent* tool_event = FindToolEvent(assistant, update->tool_call_id);
          if (tool_event == nullptr) {
            assistant->tool_events.push_back(ToolEvent{
                .call_id = update->tool_call_id,
                .tool_id = update->tool_id,
                .display_name = !update->display_name.empty() ? update->display_name
                                                              : update->tool_id,
                .arguments_summary = arguments_summary,
                .status = "Pending approval",
                .permission_decision = "pending",
                .capability_scope = !update->capability_scope.empty() ? update->capability_scope
                                                                      : update->tool_id,
                .started_at = CurrentUtcTimestamp(),
                .finished_at = {},
                .duration_ms = 0,
                .error = {},
                .output_summary = {},
            });
            tool_event = &assistant->tool_events.back();
          }

          const ToolPermissionLevel permission =
              mcp_tool_registry_.CheckPermission(update->tool_id, update->agent_id);
          const std::string capability_scope =
              !update->capability_scope.empty() ? update->capability_scope : update->tool_id;
          const bool remembered = std::any_of(
              chat.remembered_tool_approvals.begin(), chat.remembered_tool_approvals.end(),
              [&](const ChatPanelState::RememberedToolApproval& approval) {
                return approval.capability_scope == capability_scope;
              });

          chat.pending_tool_approval = ChatPanelState::PendingToolApproval{
              .conversation_id = chat.request_conversation_id,
              .assistant_message_id = chat.pending_assistant_message_id,
              .bridge_agent_id = chat.pending_bridge_agent_id,
              .bridge_request_id = chat.pending_bridge_request_id,
              .tool_call_id = update->tool_call_id,
              .tool_id = update->tool_id,
              .display_name = !update->display_name.empty() ? update->display_name
                                                            : update->tool_id,
              .arguments_json = update->arguments_json,
              .arguments_summary = arguments_summary,
              .capability_scope = capability_scope,
              .requested_ticks = SDL_GetTicks(),
              .expires_at_ticks = SDL_GetTicks() + kToolApprovalTimeoutMs,
          };

          const ToolMode tool_mode = chat.active_request.tool_mode;
          const auto finish_tool_event = [&](std::string status,
                                             std::string decision,
                                             std::string error_message,
                                             std::string output_summary) {
            if (tool_event != nullptr) {
              tool_event->status = std::move(status);
              tool_event->permission_decision = std::move(decision);
              tool_event->finished_at = CurrentUtcTimestamp();
              tool_event->duration_ms =
                  static_cast<std::int64_t>(SDL_GetTicks() - chat.pending_tool_approval->requested_ticks);
              tool_event->error = std::move(error_message);
              tool_event->output_summary = std::move(output_summary);
            }
          };

          const auto deny_tool = [&](std::string reason, std::string decision) {
            provider_bridge_manager_.SendToolDenied(
                chat.pending_bridge_agent_id, chat.pending_bridge_request_id, update->tool_call_id,
                reason);
            finish_tool_event("Denied", std::move(decision), reason, {});
            chat.pending_tool_approval.reset();
            chat.status_text = reason;
          };

          const auto run_tool = [&](std::string decision) {
            if (tool_event != nullptr) {
              tool_event->status = "Running";
              tool_event->permission_decision = decision;
            }
            std::string output_json;
            std::string error_message;
            const bool succeeded = plugin_runtime_.Host().InvokeMcpTool(
                update->tool_id, update->arguments_json, &output_json, &error_message);
            if (!succeeded) {
              deny_tool(error_message.empty() ? std::string("Tool execution failed")
                                              : error_message,
                        "failed");
              return;
            }
            provider_bridge_manager_.SendToolResult(
                chat.pending_bridge_agent_id, chat.pending_bridge_request_id, update->tool_call_id,
                output_json);
            finish_tool_event("Completed", std::move(decision), {}, ToolOutputSummary(output_json));
            chat.pending_tool_approval.reset();
            chat.status_text = "Tool completed";
          };

          if (tool_mode == ToolMode::NoTools) {
            deny_tool("Tool use is disabled for this conversation", "disabled");
          } else if (permission == ToolPermissionLevel::Denied) {
            deny_tool("Tool access denied by host policy", "denied");
          } else if (remembered) {
            run_tool("session");
          } else if (tool_mode == ToolMode::Auto &&
                     (permission == ToolPermissionLevel::Allowed ||
                      permission == ToolPermissionLevel::AllowedWithinContext)) {
            run_tool(ToolPermissionDecisionLabel(permission));
          } else {
            chat.status_text = "Waiting for tool approval";
            if (active_project &&
                (!context_.prompts.surface_visible ||
                 context_.prompts.surface.action != PromptSurfaceState::Action::ApproveChatTool)) {
              ShowPendingToolApprovalPrompt(*chat_project);
            }
          }
        }
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        RequestChromeRedraw();
        handled = true;
      } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Done) {
        const RequestStatus terminal_status = ParseBridgeTerminalStatus(update->terminal_status);
        const std::int64_t duration_ms =
            chat.request_started_ticks == 0
                ? 0
                : static_cast<std::int64_t>(SDL_GetTicks() - chat.request_started_ticks);
        if (assistant != nullptr && !update->chunk.empty()) {
          UpdateMessageContent(conversation, chat.pending_assistant_message_id, update->chunk);
        }
        if (conversation != nullptr) {
          conversation->status = terminal_status;
          conversation->last_request_duration_ms = duration_ms;
        }
        if (assistant != nullptr) {
          assistant->status = terminal_status;
          assistant->request_duration_ms = duration_ms;
          if (terminal_status != RequestStatus::Succeeded && !update->status_text.empty()) {
            assistant->error = update->status_text;
          }
        }
        if (chat.pending_tool_approval.has_value() &&
            chat.pending_tool_approval->bridge_agent_id == update->agent_id &&
            chat.pending_tool_approval->bridge_request_id == update->request_id) {
          if (ToolEvent* tool_event =
                  FindToolEvent(assistant, chat.pending_tool_approval->tool_call_id);
              tool_event != nullptr && tool_event->finished_at.empty()) {
            tool_event->status = terminal_status == RequestStatus::Cancelled ? "Cancelled"
                                                                             : "Failed";
            tool_event->permission_decision = "cancelled";
            tool_event->finished_at = CurrentUtcTimestamp();
            tool_event->duration_ms =
                static_cast<std::int64_t>(SDL_GetTicks() - chat.pending_tool_approval->requested_ticks);
            if (tool_event->error.empty()) {
              tool_event->error =
                  !update->status_text.empty() ? update->status_text : std::string("Cancelled");
            }
          }
          chat.pending_tool_approval.reset();
        }
        if (context_.prompts.surface_visible &&
            context_.prompts.surface.action == PromptSurfaceState::Action::ApproveChatTool &&
            context_.prompts.surface.bridge_agent_id == update->agent_id &&
            context_.prompts.surface.bridge_request_id == update->request_id) {
          DismissPromptSurface(true);
        }
        chat.request_in_flight = false;
        chat.request_started_ticks = 0;
        chat.status_text = !update->status_text.empty() ? update->status_text
                                                        : RequestStatusText(terminal_status);
        chat.request_conversation_id.clear();
        chat.pending_assistant_message_id.clear();
        chat.pending_bridge_agent_id.clear();
        chat.pending_bridge_request_id.clear();
        chat.active_request = ChatPanelState::RequestSnapshot{};
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        if (chat.status_text.empty()) {
          chat.status_text = RequestStatusText(terminal_status);
        }
        RequestChromeRedraw();
        handled = true;
      }
    } else {
      auto apply_inline_update = [&](ProjectWorkspaceState* project, bool active) {
        if (project == nullptr) {
          return false;
        }
        InlineCompletionState& inline_completion = project->inline_completion;
        if (!inline_completion.request_in_flight ||
            inline_completion.pending_bridge_agent_id != update->agent_id ||
            inline_completion.pending_bridge_request_id != update->request_id) {
          return false;
        }
        if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Chunk) {
          inline_completion.text += update->chunk;
        } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Done) {
          inline_completion.text += update->chunk;
          inline_completion.request_in_flight = false;
          inline_completion.visible =
              update->terminal_status == "succeeded" && !inline_completion.text.empty();
          inline_completion.error =
              update->terminal_status == "succeeded" ? std::string{} : update->status_text;
          inline_completion.pending_bridge_agent_id.clear();
          inline_completion.pending_bridge_request_id.clear();
        }
        if (active) {
          RequestFocusedEditorRedraw();
        }
        return true;
      };

      if (apply_inline_update(&context_.current_project_state, true)) {
        handled = true;
      } else {
        for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
          if (context_.HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
            continue;
          }
          if (apply_inline_update(context_.project_catalog.entries[i].get(), false)) {
            handled = true;
            break;
          }
        }
      }
    }

    if (!handled) {
      RequestChromeRedraw();
    }
  }
}

bool WorkspaceShell::InvokeMcpTool(std::string_view tool_id,
                                   std::string_view input_json,
                                   std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const ToolPermissionLevel permission = mcp_tool_registry_.CheckPermission(std::string(tool_id), "*");
  if (permission == ToolPermissionLevel::Denied) {
    if (error_message != nullptr) {
      *error_message = "Tool access denied";
    }
    return false;
  }

  std::string output_json;
  if (!plugin_runtime_.Host().InvokeMcpTool(tool_id, input_json, &output_json, error_message)) {
    return false;
  }
  output_channels_.AppendLine("mcp." + std::string(tool_id), std::string(tool_id), output_json);
  ShowOutputChannel("mcp." + std::string(tool_id));
  return true;
}

}  // namespace microide::workspace
