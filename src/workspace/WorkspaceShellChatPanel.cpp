#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <limits>

#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

std::string SerializeChatComposerViewport(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines(), util::LineEnding::LF);
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

bool AgentSupportsCapability(const ExternalAgentSpec* agent, std::string_view capability) {
  if (agent == nullptr) {
    return false;
  }
  return std::find(agent->capabilities.begin(), agent->capabilities.end(), capability) !=
         agent->capabilities.end();
}

}  // namespace

void WorkspaceShell::ShowChatPanel() {
  if (context_.current_project_state.panel.chat.conversation_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
  }
  LoadChatComposerDraft();
  context_.current_project_state.sidebar.view_id = "chat";
  context_.current_project_state.sidebar.visible = true;
  context_.current_project_state.sidebar.temporary = false;
  context_.current_project_state.sidebar.prev_view_id.clear();
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  context_.current_project_state.surface.focus = FocusTarget::Sidebar;
  RequestSidebarRedraw();
}

Conversation* WorkspaceShell::ActiveConversation() {
  return context_.current_project_state.conversations.GetConversation(
      context_.current_project_state.panel.chat.conversation_id);
}

const Conversation* WorkspaceShell::ActiveConversation() const {
  return context_.current_project_state.conversations.GetConversation(
      context_.current_project_state.panel.chat.conversation_id);
}

bool WorkspaceShell::ActivateChatConversation(std::string_view conversation_id) {
  if (conversation_id.empty() ||
      context_.current_project_state.conversations.GetConversation(std::string(conversation_id)) ==
          nullptr) {
    return false;
  }
  SyncActiveConversationDraft();
  context_.current_project_state.panel.chat.conversation_id = std::string(conversation_id);
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::SyncActiveConversationDraft() {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  conversation->draft = ChatComposerText();
}

void WorkspaceShell::LoadChatComposerDraft() {
  const Conversation* conversation = ActiveConversation();
  LoadChatComposerViewport(&context_.current_project_state.panel.chat.composer,
                          conversation != nullptr ? std::string_view(conversation->draft)
                                                 : std::string_view{});
}

bool WorkspaceShell::CreateChatConversation() {
  SyncActiveConversationDraft();
  context_.current_project_state.panel.chat.conversation_id =
      context_.current_project_state.conversations.CreateConversation(
          NextConversationTitle(context_.current_project_state.conversations), std::string{});
  context_.current_project_state.panel.chat.scroll_row = 0;
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::DeleteActiveChatConversation() {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return false;
  }
  const std::string current_id = conversation->id;
  if (context_.current_project_state.panel.chat.request_in_flight &&
      context_.current_project_state.panel.chat.request_conversation_id == current_id) {
    CancelActiveChatRequest();
  }

  const auto& conversations = context_.current_project_state.conversations.conversations();
  std::string next_id;
  for (const Conversation& item : conversations) {
    if (item.id != current_id) {
      next_id = item.id;
      break;
    }
  }
  context_.current_project_state.conversations.DeleteConversation(current_id);
  if (next_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
  } else {
    context_.current_project_state.panel.chat.conversation_id = next_id;
  }
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

std::vector<const AiProviderSpec*> WorkspaceShell::ChatProviders() const {
  std::vector<const AiProviderSpec*> providers;
  for (const AiProviderSpec& provider : ai_provider_registry_.Specs()) {
    const ExternalAgentSpec* agent = external_agent_registry_.FindAgent(provider.id);
    if (AgentSupportsCapability(agent, "chat")) {
      providers.push_back(&provider);
    }
  }
  return providers;
}

std::vector<std::string> WorkspaceShell::ChatModelsForConversation(
    const Conversation& conversation) const {
  std::vector<std::string> models = provider_bridge_manager_.GetModels(conversation.provider_id);
  if (models.empty()) {
    if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(conversation.provider_id);
        provider != nullptr) {
      models = provider->models;
    }
  }
  if (!conversation.model_id.empty() &&
      std::find(models.begin(), models.end(), conversation.model_id) == models.end()) {
    models.push_back(conversation.model_id);
  }
  return models;
}

std::string WorkspaceShell::ChatComposerText() const {
  return SerializeChatComposerViewport(context_.current_project_state.panel.chat.composer);
}

void WorkspaceShell::CycleActiveConversationProvider(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  const auto providers = ChatProviders();
  if (providers.empty()) {
    conversation->provider_id.clear();
    conversation->model_id.clear();
    RequestSidebarRedraw();
    RequestChromeRedraw();
    return;
  }

  auto it = std::find_if(providers.begin(), providers.end(),
                         [&](const AiProviderSpec* provider) {
                           return provider != nullptr && provider->id == conversation->provider_id;
                         });
  std::size_t index = it == providers.end() ? 0 : static_cast<std::size_t>(it - providers.begin());
  const int count = static_cast<int>(providers.size());
  index = static_cast<std::size_t>((static_cast<int>(index) + delta + count * 8) % count);
  conversation->provider_id = providers[index]->id;
  conversation->model_id.clear();
  RequestSidebarRedraw();
  RequestChromeRedraw();
}

void WorkspaceShell::CycleActiveConversationModel(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr || conversation->provider_id.empty()) {
    return;
  }
  const std::vector<std::string> models = ChatModelsForConversation(*conversation);
  if (models.empty()) {
    conversation->model_id.clear();
    RequestSidebarRedraw();
    return;
  }
  auto it = std::find(models.begin(), models.end(), conversation->model_id);
  std::size_t index = it == models.end() ? 0 : static_cast<std::size_t>(it - models.begin());
  const int count = static_cast<int>(models.size());
  index = static_cast<std::size_t>((static_cast<int>(index) + delta + count * 8) % count);
  conversation->model_id = models[index];
  RequestSidebarRedraw();
}

void WorkspaceShell::CycleActiveConversationToolMode(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  const int count = 3;
  const int current =
      conversation->tool_mode == ToolMode::NoTools ? 0
      : conversation->tool_mode == ToolMode::Ask   ? 1
                                                   : 2;
  const int next = (current + delta + count * 4) % count;
  conversation->tool_mode =
      next == 0 ? ToolMode::NoTools : next == 1 ? ToolMode::Ask : ToolMode::Auto;
  RequestSidebarRedraw();
}

std::string WorkspaceShell::ChatAuthBannerText(const Conversation* conversation) const {
  if (ai_provider_registry_.Specs().empty()) {
    return "No chat providers are available.";
  }
  if (conversation == nullptr || conversation->provider_id.empty()) {
    return "Select a provider to send chat requests.";
  }

  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(conversation->provider_id);
  if (provider == nullptr) {
    return "The selected provider is no longer available.";
  }

  switch (GetProviderAuthStatus(provider->id)) {
    case ProviderAuthStatus::KeyMissing:
      return "Add an API key for " + provider->label + " before sending.";
    case ProviderAuthStatus::KeyInvalid:
      return provider->label + " rejected the stored API key.";
    case ProviderAuthStatus::KeyPresent:
      return "Stored API key for " + provider->label + " has not been validated yet.";
    case ProviderAuthStatus::KeyValid:
    case ProviderAuthStatus::Unknown:
      break;
  }

  const std::vector<std::string> models = ChatModelsForConversation(*conversation);
  if (!conversation->model_id.empty() &&
      !models.empty() &&
      std::find(models.begin(), models.end(), conversation->model_id) == models.end()) {
    return "The selected model is no longer offered by " + provider->label + ".";
  }
  return {};
}

}  // namespace microide::workspace
