#include "plugin/PluginDiscoveryInterop.h"

#include <algorithm>
#include <set>
#include <string_view>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"

namespace microide::plugin::discovery_interop {
namespace {

std::filesystem::path GlobalPluginDirectory() {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "plugins";
}

bool ShouldSkipPluginDirectoryName(std::string_view name) {
  return name.ends_with(".bak") || name.find(".bak-") != std::string_view::npos;
}

}  // namespace

std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots(
    const std::filesystem::path& /*current_project_root*/) {
  std::vector<std::pair<std::filesystem::path, bool>> plugin_roots;
  std::set<std::string> seen_directory_names;
  const auto append = [&](const std::filesystem::path& plugins_dir, bool project_local) {
    if (plugins_dir.empty()) {
      return;
    }
    if (platform::ReadPathType(plugins_dir) != platform::PathType::Directory) {
      return;
    }

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
    for (const auto& path : entries) {
      plugin_roots.emplace_back(path, project_local);
    }
  };

  append(GlobalPluginDirectory(), false);
  return plugin_roots;
}

}  // namespace microide::plugin::discovery_interop
