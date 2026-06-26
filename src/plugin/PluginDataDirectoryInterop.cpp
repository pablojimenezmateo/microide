#include "plugin/PluginDataDirectoryInterop.h"

#include "platform/Filesystem.h"

namespace microide::plugin::data_directory_interop {

std::vector<std::filesystem::path> DataDirectories(
    std::string_view subdirectory,
    const std::vector<std::filesystem::path>& plugin_roots) {
  if (subdirectory.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> directories;
  directories.reserve(plugin_roots.size());
  for (const auto& root : plugin_roots) {
    const std::filesystem::path candidate = (root / subdirectory).lexically_normal();
    if (platform::ReadPathType(candidate) != platform::PathType::Directory) {
      continue;
    }
    directories.push_back(candidate);
  }
  return directories;
}

}  // namespace microide::plugin::data_directory_interop
