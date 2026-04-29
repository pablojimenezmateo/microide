#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

namespace {

std::string ProviderApiKeySettingId(const AiProviderSpec& provider) {
  if (!provider.plugin_id.empty()) {
    return provider.plugin_id + ".api_key";
  }
  const std::size_t separator = provider.id.find('.');
  const std::string prefix = separator == std::string::npos
                                 ? provider.id
                                 : provider.id.substr(0, separator);
  return prefix.empty() ? std::string{} : prefix + ".api_key";
}

}  // namespace

std::string WorkspaceShell::ProviderApiKeyStorageKey(const AiProviderSpec& provider) const {
  return !provider.api_key_name.empty() ? provider.api_key_name : provider.id + ".api_key";
}

std::optional<std::string> WorkspaceShell::ResolveProviderApiKey(
    const AiProviderSpec& provider) const {
  if (const std::optional<std::string> stored =
          secret_storage_.Retrieve(ProviderApiKeyStorageKey(provider));
      stored.has_value() && !stored->empty()) {
    return stored;
  }
  const std::string setting_id = ProviderApiKeySettingId(provider);
  if (setting_id.empty()) {
    return std::nullopt;
  }
  if (const std::optional<std::string> configured = GetSettingValue(setting_id);
      configured.has_value() && !configured->empty()) {
    return configured;
  }
  return std::nullopt;
}

bool WorkspaceShell::SetProviderApiKey(std::string_view provider_id,
                                       std::string_view api_key,
                                       std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  if (provider == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown provider: " + std::string(provider_id);
    }
    return false;
  }
  const std::string key = ProviderApiKeyStorageKey(*provider);
  if (!secret_storage_.Store(key, std::string(api_key))) {
    if (error_message != nullptr) {
      *error_message = "Failed to store API key for " + std::string(provider_id);
    }
    return false;
  }
  // If there is a running bridge for this provider, restart it with the new key.
  const ExternalAgentSpec* agent =
      external_agent_registry_.FindAgent(std::string(provider_id));
  if (agent != nullptr && !agent->command.empty()) {
    provider_bridge_manager_.StartBridge(agent->id,
                                         agent->command,
                                         std::string(api_key),
                                         context_.current_project_state.root);
  }
  return true;
}

bool WorkspaceShell::ClearProviderApiKey(std::string_view provider_id,
                                         std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  const std::string key = provider != nullptr ? ProviderApiKeyStorageKey(*provider)
                                               : std::string(provider_id) + ".api_key";
  secret_storage_.Delete(key);
  provider_bridge_manager_.StopBridge(std::string(provider_id));
  return true;
}

ProviderAuthStatus WorkspaceShell::GetProviderAuthStatus(std::string_view provider_id) const {
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  if (provider == nullptr) {
    return ProviderAuthStatus::Unknown;
  }
  if (!ResolveProviderApiKey(*provider).has_value()) {
    return ProviderAuthStatus::KeyMissing;
  }
  const ProviderAuthStatus bridge_status =
      provider_bridge_manager_.GetAuthStatus(std::string(provider_id));
  if (bridge_status != ProviderAuthStatus::Unknown &&
      bridge_status != ProviderAuthStatus::KeyMissing) {
    return bridge_status;
  }
  return ProviderAuthStatus::KeyPresent;
}

}  // namespace microide::workspace
