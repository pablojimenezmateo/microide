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
  const AiRuntimeLaunchContext launch_context{
      .cwd = context_.current_project_state.root,
      .secret = std::string(api_key),
  };
  (void)ai_provider_runtime_service_.RequestAuthCheck(provider->id, launch_context, nullptr);
  (void)ai_provider_runtime_service_.RequestModelList(provider->id, launch_context, nullptr);
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
  ai_provider_runtime_service_.StopRuntime(provider_id);
  return true;
}

ProviderAuthStatus WorkspaceShell::GetProviderAuthStatus(std::string_view provider_id) const {
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  if (provider == nullptr) {
    return ProviderAuthStatus::Unknown;
  }
  const ProviderAuthStatus runtime_status = ai_provider_runtime_service_.GetAuthStatus(provider->id);
  if (!ResolveProviderApiKey(*provider).has_value()) {
    if (provider->type == "external" && provider->api_key_name.empty()) {
      return runtime_status == ProviderAuthStatus::Unknown ? ProviderAuthStatus::KeyPresent
                                                           : runtime_status;
    }
    return ProviderAuthStatus::KeyMissing;
  }
  if (runtime_status != ProviderAuthStatus::Unknown &&
      runtime_status != ProviderAuthStatus::KeyMissing) {
    return runtime_status;
  }
  return ProviderAuthStatus::KeyPresent;
}

}  // namespace microide::workspace
