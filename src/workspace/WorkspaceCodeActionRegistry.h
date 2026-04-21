#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Code action provider: Lua function that returns code actions for a range.
struct CodeActionProviderSpec {
  std::string id;
  std::string plugin_id;
  std::string language_id;
};

// Registry for code action providers (Lua-driven).
class CodeActionRegistry {
 public:
  CodeActionRegistry();
  ~CodeActionRegistry();

  void Register(const CodeActionProviderSpec& spec);
  const std::vector<CodeActionProviderSpec>& Specs() const { return specs_; }

  // Find provider for language_id; returns first if multiple.
  const CodeActionProviderSpec* FindProvider(const std::string& language_id) const;

 private:
  std::vector<CodeActionProviderSpec> specs_;
};

}  // namespace microide::workspace
