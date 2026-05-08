#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

void WorkspaceShell::ShowChatPanel() {}

Conversation* WorkspaceShell::ActiveConversation() { return nullptr; }

const Conversation* WorkspaceShell::ActiveConversation() const { return nullptr; }

bool WorkspaceShell::ActivateChatConversation(std::string_view conversation_id) {
  (void)conversation_id;
  return false;
}

void WorkspaceShell::SyncActiveConversationDraft() {}

void WorkspaceShell::LoadChatComposerDraft() {}

bool WorkspaceShell::CreateChatConversation() { return false; }

bool WorkspaceShell::DeleteActiveChatConversation() { return false; }

bool WorkspaceShell::CancelActiveChatRequest() { return false; }

bool WorkspaceShell::RetryActiveChatRequest(std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Chat request support is retired";
  }
  return false;
}

std::vector<const AiProviderSpec*> WorkspaceShell::ChatProviders() const { return {}; }

std::vector<std::string> WorkspaceShell::ChatModelsForConversation(
    const Conversation& conversation) const {
  (void)conversation;
  return {};
}

void WorkspaceShell::CycleActiveConversationProvider(int delta) { (void)delta; }

void WorkspaceShell::CycleActiveConversationModel(int delta) { (void)delta; }

void WorkspaceShell::CycleActiveConversationToolMode(int delta) { (void)delta; }

std::string WorkspaceShell::ChatComposerText() const { return {}; }

std::string WorkspaceShell::ChatAuthBannerText(const Conversation* conversation) const {
  (void)conversation;
  return {};
}

bool WorkspaceShell::StartChatRequest(std::string message, std::string* error_message) {
  (void)message;
  if (error_message != nullptr) {
    *error_message = "Chat request support is retired";
  }
  return false;
}

bool WorkspaceShell::SetProviderApiKey(std::string_view provider_id,
                                       std::string_view api_key,
                                       std::string* error_message) {
  (void)provider_id;
  (void)api_key;
  if (error_message != nullptr) {
    *error_message = "Provider auth is retired";
  }
  return false;
}

bool WorkspaceShell::ClearProviderApiKey(std::string_view provider_id, std::string* error_message) {
  (void)provider_id;
  if (error_message != nullptr) {
    *error_message = "Provider auth is retired";
  }
  return false;
}

ProviderAuthStatus WorkspaceShell::GetProviderAuthStatus(std::string_view provider_id) const {
  (void)provider_id;
  return ProviderAuthStatus::KeyMissing;
}

std::string WorkspaceShell::ProviderApiKeyStorageKey(const AiProviderSpec& provider) const {
  (void)provider;
  return {};
}

std::optional<std::string> WorkspaceShell::ResolveProviderApiKey(
    const AiProviderSpec& provider) const {
  (void)provider;
  return std::nullopt;
}

void WorkspaceShell::ConsumeAiRuntimeUpdates() {}

void WorkspaceShell::ExpirePendingToolApprovals() {}

bool WorkspaceShell::ResolveChatToolApprovalPrompt(bool allow, bool remember_for_session) {
  (void)allow;
  (void)remember_for_session;
  return false;
}

void WorkspaceShell::ShowPendingToolApprovalPrompt(ProjectWorkspaceState& project) {
  (void)project;
}

WorkspaceShell::ChatTranscriptLayout WorkspaceShell::BuildChatTranscriptLayout(
    const SDL_FRect& sidebar_rect) const {
  (void)sidebar_rect;
  return {};
}

std::size_t WorkspaceShell::ChatTranscriptLineCount(const SDL_FRect& sidebar_rect) const {
  (void)sidebar_rect;
  return 0;
}

bool WorkspaceShell::HasChatTranscriptLinkAtPoint(const SDL_FRect& sidebar_rect, float x, float y) const {
  (void)sidebar_rect;
  (void)x;
  (void)y;
  return false;
}

bool WorkspaceShell::ActivateChatTranscriptLinkAtPoint(const SDL_FRect& sidebar_rect, float x, float y) {
  (void)sidebar_rect;
  (void)x;
  (void)y;
  return false;
}

std::vector<std::string> WorkspaceShell::ChatTranscriptDebugLines(const SDL_FRect& sidebar_rect) const {
  (void)sidebar_rect;
  return {};
}

std::optional<SDL_FRect> WorkspaceShell::FindChatTranscriptLinkRect(
    const SDL_FRect& sidebar_rect,
    std::string_view match) const {
  (void)sidebar_rect;
  (void)match;
  return std::nullopt;
}

}  // namespace microide::workspace
