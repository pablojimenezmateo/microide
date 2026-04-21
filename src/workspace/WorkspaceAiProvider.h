#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// AI provider: language model provider (OpenAI, Anthropic, local, etc.)
struct AiProviderSpec {
  std::string id;
  std::string label;
  std::string type;  // "cloud", "local", "external"
  std::string api_key_name;  // secret storage key for API key
  std::vector<std::string> models;  // available models
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
