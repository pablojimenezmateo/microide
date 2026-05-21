#include "plugin/PluginDiscoveryInterop.h"

#include <algorithm>
#include <set>
#include <string_view>

#include "platform/Filesystem.h"
#include "plugin/PluginInstallRoot.h"

namespace microide::plugin::discovery_interop {
namespace {

bool ShouldSkipPluginDirectoryName(std::string_view name) {
  return name.ends_with(".bak") || name.find(".bak-") != std::string_view::npos;
}

}  // namespace

std::vector<std::filesystem::path> DiscoverPluginRoots() {
  const std::filesystem::path plugins_dir = ResolveUserPluginInstallRoot();
  if (plugins_dir.empty() ||
      platform::ReadPathType(plugins_dir) != platform::PathType::Directory) {
    return {};
  }

  std::vector<std::filesystem::path> plugin_roots;
  std::set<std::string> seen_directory_names;
  std::vector<std::filesystem::path> entries;
  for (const auto& entry : platform::ListDirectory(plugins_dir)) {
    if (entry.type != platform::PathType::Directory) {
      continue;
    }
    const std::string directory_name = entry.path.filename().string();
    if (ShouldSkipPluginDirectoryName(directory_name) ||
        seen_directory_names.contains(directory_name)) {
      continue;
    }
    const std::filesystem::path init_path = entry.path / "init.lua";
    if (platform::ReadPathType(init_path) == platform::PathType::RegularFile) {
      seen_directory_names.insert(directory_name);
      entries.push_back(entry.path.lexically_normal());
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.filename() < rhs.filename(); });
  plugin_roots.insert(plugin_roots.end(), entries.begin(), entries.end());
  return plugin_roots;
}

}  // namespace microide::plugin::discovery_interop
