#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Capability flags reported by or inferred for an AI provider.
struct ProviderCapabilities {
  bool chat = false;
  bool streaming = false;
  bool tool_call = false;
  bool system_prompt = false;
  bool model_enumeration = false;
  bool structured_output = false;
  bool image_attachment = false;
};

// Credential state for a provider's API key.
enum class ProviderAuthStatus {
  Unknown,     // never checked
  KeyMissing,  // no key stored
  KeyPresent,  // key stored, not yet validated
  KeyValid,    // bridge confirmed key is accepted
  KeyInvalid,  // bridge returned an auth failure
};

// AI provider: language model provider (OpenAI, Anthropic, local, etc.)
struct AiProviderSpec {
  std::string id;
  std::string label;
  std::string type;  // "cloud", "local", "external"
  std::string api_key_name;  // secret storage key for API key
  std::vector<std::string> models;  // static model list from plugin
  std::string runtime;  // "sidecar", "openai_compat", "anthropic_messages"
  std::string base_url;
  std::string default_model;
  ProviderCapabilities capabilities;
  std::string plugin_id;
};

// AI provider registry: manages LLM provider registrations.
class AiProviderRegistry {
 public:
  AiProviderRegistry();
  ~AiProviderRegistry();

  void Register(const AiProviderSpec& spec);
  const std::vector<AiProviderSpec>& Specs() const { return specs_; }

  // Find provider by id.
  const AiProviderSpec* FindProvider(const std::string& id) const;

  // Get all available models across all providers.
  std::vector<std::pair<std::string, std::string>> AllModels() const;

 private:
  std::vector<AiProviderSpec> specs_;
};

}  // namespace microide::workspace
