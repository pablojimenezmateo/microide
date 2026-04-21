#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// SCM provider: source control system plugin (Git, Hg, Perforce, etc.)
struct ScmProviderSpec {
  std::string id;
  std::string label;
  std::string plugin_id;
};

// SCM registry: manages source control provider registrations.
class ScmRegistry {
 public:
  ScmRegistry();
  ~ScmRegistry();

  void Register(const ScmProviderSpec& spec);
  const std::vector<ScmProviderSpec>& Specs() const { return specs_; }

  // Find provider by id.
  const ScmProviderSpec* FindProvider(const std::string& id) const;

 private:
  std::vector<ScmProviderSpec> specs_;
};

}  // namespace microide::workspace
