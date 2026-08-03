#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

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

using ToolRegistry = ProviderRegistry<ToolSpec>;

// First tool matching `id` on `platform` (or nullptr).
inline const ToolSpec* FindTool(const ToolRegistry& registry, std::string_view id,
                                std::string_view platform) {
  return registry.FindIf(
      [&](const ToolSpec& spec) { return spec.id == id && spec.platform == platform; });
}

}  // namespace microide::workspace
