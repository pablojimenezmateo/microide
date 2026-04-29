#include "plugin/PluginDiscoveryInterop.h"

#include <algorithm>
#include <set>
#include <string_view>

#include <SDL3/SDL.h>

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

#ifndef MICROIDE_TESTING
std::filesystem::path RepoPluginDirectory() {
  const auto repo_plugins_from_root = [](const std::filesystem::path& start) {
    if (start.empty()) {
      return std::filesystem::path{};
    }
    std::error_code error;
    std::filesystem::path current = std::filesystem::weakly_canonical(start, error);
    if (error) {
      current = start.lexically_normal();
    }
    while (!current.empty()) {
      const std::filesystem::path plugins_dir = current / "plugins";
      if (platform::ReadPathType(plugins_dir) == platform::PathType::Directory &&
          platform::ReadPathType(plugins_dir / "README.md") == platform::PathType::RegularFile) {
        return plugins_dir.lexically_normal();
      }
      const std::filesystem::path parent = current.parent_path();
      if (parent == current) {
        break;
      }
      current = parent;
    }
    return std::filesystem::path{};
  };

  if (const char* raw_base_path = SDL_GetBasePath();
      raw_base_path != nullptr && raw_base_path[0] != '\0') {
    if (const std::filesystem::path plugins_dir =
            repo_plugins_from_root(std::filesystem::path(raw_base_path));
        !plugins_dir.empty()) {
      return plugins_dir;
    }
  }

  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  return error ? std::filesystem::path{} : repo_plugins_from_root(cwd);
}
#endif

}  // namespace

std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots(
    const std::filesystem::path& current_project_root) {
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
#ifndef MICROIDE_TESTING
  append(RepoPluginDirectory(), false);
#endif
  if (!current_project_root.empty()) {
    append(current_project_root / ".microide" / "plugins", true);
  }
  return plugin_roots;
}

}  // namespace microide::plugin::discovery_interop
