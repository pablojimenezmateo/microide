#include "plugin/PluginDataDirectoryInterop.h"

#include "platform/Filesystem.h"

namespace microide::plugin::data_directory_interop {

std::vector<std::filesystem::path> DataDirectories(
    std::string_view subdirectory,
    const std::vector<runtime_types::PluginInstance>& plugins) {
  if (subdirectory.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> directories;
  directories.reserve(plugins.size());

  const auto append_matching_directories = [&](bool project_local) {
    for (const auto& plugin : plugins) {
      if (plugin.project_local != project_local) {
        continue;
      }
      const std::filesystem::path candidate = (plugin.root / subdirectory).lexically_normal();
      if (platform::ReadPathType(candidate) != platform::PathType::Directory) {
        continue;
      }
      directories.push_back(candidate);
    }
  };

  append_matching_directories(true);
  append_matching_directories(false);
  return directories;
}

}  // namespace microide::plugin::data_directory_interop
