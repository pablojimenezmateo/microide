#pragma once

#include <string>
#include <string_view>

#include "workspace/ProviderRegistry.h"

namespace microide::workspace {

struct ToolSpec {
  std::string id;
  std::string plugin_id;
  std::string label;
  std::string platform;
  std::string download_url;
  std::string sha256;
  std::string install_dir;
};

using ToolRegistry = ProviderRegistry<ToolSpec>;

inline const ToolSpec* FindTool(const ToolRegistry& registry, std::string_view id,
                                std::string_view platform) {
  return registry.FindIf(
      [&](const ToolSpec& spec) { return spec.id == id && spec.platform == platform; });
}

}
