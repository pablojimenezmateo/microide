#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Tool: downloadable executable for a platform (e.g., LSP server binary).
struct ToolSpec {
  std::string id;
  std::string plugin_id;
  std::string label;
  std::string platform;  // "linux", "macos", "windows"
  std::string download_url;
  std::string sha256;    // for verification
  std::string install_dir;  // relative to plugin dir or cache
};

// Registry for downloadable tools.
class ToolRegistry {
 public:
  ToolRegistry();
  ~ToolRegistry();

  void Register(const ToolSpec& spec);
  const std::vector<ToolSpec>& Specs() const { return specs_; }

  // Find tool by id and platform.
  const ToolSpec* FindTool(const std::string& id, const std::string& platform) const;

 private:
  std::vector<ToolSpec> specs_;
};

}  // namespace microide::workspace
