#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>

namespace microide::workspace {

namespace {

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

}  // namespace

bool WorkspaceShell::CancelActiveChatRequest() {
  ChatPanelState& chat = context_.current_project_state.panel.chat;
  Conversation* conversation =
      context_.current_project_state.conversations.GetConversation(chat.request_conversation_id);
  if (conversation == nullptr || !chat.request_in_flight || chat.pending_assistant_message_id.empty()) {
    return false;
  }

  ai_provider_runtime_service_.CancelRequest(chat.pending_provider_id, chat.pending_request_id);
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
      context_.prompts.surface.provider_id == chat.pending_provider_id &&
      context_.prompts.surface.request_id == chat.pending_request_id) {
    DismissPromptSurface(false);
  }
  chat.pending_tool_approval.reset();
  chat.request_in_flight = false;
  chat.request_started_ticks = 0;
  chat.status_text = "Cancelled";
  chat.request_conversation_id.clear();
  chat.pending_assistant_message_id.clear();
  chat.pending_provider_id.clear();
  chat.pending_request_id.clear();
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

  std::string provider_id = conversation != nullptr ? conversation->provider_id : std::string{};
  if (provider_id.empty() ||
      !ai_provider_runtime_service_.ProviderSupportsWorkflow(provider_id, AiRuntimeWorkflow::Chat)) {
    const std::vector<std::string> provider_ids =
        ai_provider_runtime_service_.ProviderIdsForWorkflow(AiRuntimeWorkflow::Chat);
    provider_id = provider_ids.empty() ? std::string{} : provider_ids.front();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(provider_id);
  if (provider == nullptr) {
    if (error_message != nullptr) {
      *error_message = "No chat provider runtime is available";
    }
    return false;
  }

  const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
  if (auth_status == ProviderAuthStatus::KeyMissing ||
      auth_status == ProviderAuthStatus::KeyInvalid) {
    if (error_message != nullptr) {
      *error_message = ChatAuthBannerText(conversation);
    }
    return false;
  }

  const std::string user_id = GenerateRuntimeMessageId("user");
  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  conversation->provider_id = provider->id;
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
    assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : provider->id;
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
      .provider_id = provider->id,
      .model_id = conversation != nullptr ? conversation->model_id : std::string{},
      .tool_mode = conversation != nullptr ? conversation->tool_mode : ToolMode::NoTools,
      .context_policy = ai_context_manager_.policy(),
      .context_items = ai_context_manager_.GetContext(),
  };
  context_.current_project_state.panel.chat.pending_tool_approval.reset();
  context_.current_project_state.panel.chat.status_text = "Waiting for " + provider->label;
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

  std::vector<AiRuntimeMessage> runtime_messages;
  if (conversation != nullptr) {
    for (const auto& msg : conversation->messages) {
      if (msg.role == MessageRole::User && msg.id != user_id) {
        runtime_messages.push_back(AiRuntimeMessage{.role = "user", .content = msg.content});
      } else if (msg.role == MessageRole::Assistant &&
                 msg.status == RequestStatus::Succeeded) {
        runtime_messages.push_back(AiRuntimeMessage{.role = "assistant", .content = msg.content});
      } else if (msg.id == user_id) {
        runtime_messages.push_back(AiRuntimeMessage{.role = "user", .content = msg.content});
        break;
      }
    }
  }
  if (runtime_messages.empty() || runtime_messages.back().role != "user") {
    runtime_messages.push_back(AiRuntimeMessage{.role = "user", .content = message});
  }

  const std::string request_id = GenerateRuntimeMessageId("runtime-req");
  const std::string model_id = conversation != nullptr ? conversation->model_id : "";
  const std::string system_prompt = conversation != nullptr ? conversation->system_prompt : "";
  const ToolMode tool_mode = conversation != nullptr ? conversation->tool_mode : ToolMode::NoTools;
  const std::string tool_mode_str = SerializeToolMode(tool_mode);
  std::vector<AiRuntimeToolSpec> runtime_tools;
  if (tool_mode != ToolMode::NoTools) {
    for (const McpToolSpec* tool : mcp_tool_registry_.GetAvailableTools(provider->id)) {
      if (tool == nullptr) {
        continue;
      }
      runtime_tools.push_back(AiRuntimeToolSpec{
          .id = tool->id,
          .display_name = !tool->name.empty() ? tool->name : tool->id,
          .description = tool->description,
          .input_schema = tool->input_schema,
      });
    }
  }

  context_.current_project_state.panel.chat.pending_provider_id = provider->id;
  context_.current_project_state.panel.chat.pending_request_id = request_id;
  const AiRuntimeLaunchContext launch_context{
      .cwd = context_.current_project_state.root,
      .secret = ResolveProviderApiKey(*provider),
  };
  if (!ai_provider_runtime_service_.StartRequest(
          AiRuntimeRequest{
              .workflow = AiRuntimeWorkflow::Chat,
              .provider_id = provider->id,
              .request_id = request_id,
              .messages = std::move(runtime_messages),
              .model_id = model_id,
              .system_prompt = system_prompt,
              .tool_mode = tool_mode_str,
              .tools = std::move(runtime_tools),
          },
          launch_context, error_message)) {
    const std::string failure_reason =
        error_message == nullptr || error_message->empty()
            ? std::string("Failed to send chat request to provider runtime")
            : *error_message;
    if (error_message != nullptr) {
      *error_message = failure_reason;
    }
    mark_failed(failure_reason);
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_provider_id.clear();
    context_.current_project_state.panel.chat.pending_request_id.clear();
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

  std::string provider_id = conversation->provider_id;
  if (provider_id.empty() ||
      !ai_provider_runtime_service_.ProviderSupportsWorkflow(provider_id, AiRuntimeWorkflow::Chat)) {
    const std::vector<std::string> provider_ids =
        ai_provider_runtime_service_.ProviderIdsForWorkflow(AiRuntimeWorkflow::Chat);
    provider_id = provider_ids.empty() ? std::string{} : provider_ids.front();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(provider_id);
  if (provider == nullptr) {
    if (error_message != nullptr) {
      *error_message = "No chat provider runtime is available";
    }
    return false;
  }

  const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
  if (auth_status == ProviderAuthStatus::KeyMissing ||
      auth_status == ProviderAuthStatus::KeyInvalid) {
    if (error_message != nullptr) {
      *error_message = ChatAuthBannerText(conversation);
    }
    return false;
  }

  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  Message assistant_msg;
  assistant_msg.id = assistant_id;
  assistant_msg.role = MessageRole::Assistant;
  assistant_msg.timestamp = CurrentUtcTimestamp();
  assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : provider->id;
  assistant_msg.status = RequestStatus::Running;
  context_.current_project_state.conversations.AddMessage(conversation->id, assistant_msg);

  conversation->status = RequestStatus::Running;
  conversation->last_request_duration_ms = 0;
  context_.current_project_state.panel.chat.request_conversation_id = conversation->id;
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.request_started_ticks = SDL_GetTicks();
  context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{
      .provider_id = provider->id,
      .model_id = conversation->model_id,
      .tool_mode = conversation->tool_mode,
      .context_policy = ai_context_manager_.policy(),
      .context_items = ai_context_manager_.GetContext(),
  };
  context_.current_project_state.panel.chat.pending_tool_approval.reset();
  context_.current_project_state.panel.chat.status_text = "Retrying with " + provider->label;
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

  std::vector<AiRuntimeMessage> runtime_messages;
  for (const auto& msg : conversation->messages) {
    if (msg.role == MessageRole::User) {
      runtime_messages.push_back(AiRuntimeMessage{.role = "user", .content = msg.content});
      if (msg.id == last_user_it->id) {
        break;
      }
    } else if (msg.role == MessageRole::Assistant &&
               msg.status == RequestStatus::Succeeded) {
      runtime_messages.push_back(AiRuntimeMessage{.role = "assistant", .content = msg.content});
    }
  }
  if (runtime_messages.empty() || runtime_messages.back().role != "user") {
    runtime_messages.push_back(AiRuntimeMessage{.role = "user", .content = message});
  }

  const std::string request_id = GenerateRuntimeMessageId("runtime-req");
  const std::string tool_mode_str = SerializeToolMode(conversation->tool_mode);
  std::vector<AiRuntimeToolSpec> runtime_tools;
  if (conversation->tool_mode != ToolMode::NoTools) {
    for (const McpToolSpec* tool : mcp_tool_registry_.GetAvailableTools(provider->id)) {
      if (tool == nullptr) {
        continue;
      }
      runtime_tools.push_back(AiRuntimeToolSpec{
          .id = tool->id,
          .display_name = !tool->name.empty() ? tool->name : tool->id,
          .description = tool->description,
          .input_schema = tool->input_schema,
      });
    }
  }
  context_.current_project_state.panel.chat.pending_provider_id = provider->id;
  context_.current_project_state.panel.chat.pending_request_id = request_id;
  const AiRuntimeLaunchContext launch_context{
      .cwd = context_.current_project_state.root,
      .secret = ResolveProviderApiKey(*provider),
  };
  if (!ai_provider_runtime_service_.StartRequest(
          AiRuntimeRequest{
              .workflow = AiRuntimeWorkflow::Chat,
              .provider_id = provider->id,
              .request_id = request_id,
              .messages = std::move(runtime_messages),
              .model_id = conversation->model_id,
              .system_prompt = conversation->system_prompt,
              .tool_mode = tool_mode_str,
              .tools = std::move(runtime_tools),
          },
          launch_context, error_message)) {
    const std::string failure_reason =
        error_message == nullptr || error_message->empty()
            ? std::string("Failed to send chat request to provider runtime")
            : *error_message;
    if (error_message != nullptr) {
      *error_message = failure_reason;
    }
    mark_failed(failure_reason);
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_provider_id.clear();
    context_.current_project_state.panel.chat.pending_request_id.clear();
    context_.current_project_state.panel.chat.active_request = ChatPanelState::RequestSnapshot{};
    return false;
  }

  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}


}  // namespace microide::workspace
