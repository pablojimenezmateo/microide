#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Completion provider: Lua function that returns completion items.
struct CompletionProviderSpec {
  std::string id;
  std::string plugin_id;
  std::string language_id;
  std::string trigger_characters;  // e.g., "." or "->."
};

// Registry for completion providers (Lua-driven).
class CompletionRegistry {
 public:
  CompletionRegistry();
  ~CompletionRegistry();

  void Register(const CompletionProviderSpec& spec);
  const std::vector<CompletionProviderSpec>& Specs() const { return specs_; }

  // Find provider for language_id; returns first if multiple.
  const CompletionProviderSpec* FindProvider(const std::string& language_id) const;

 private:
  std::vector<CompletionProviderSpec> specs_;
};

}  // namespace microide::workspace
