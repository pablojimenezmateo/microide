#pragma once

#include <filesystem>
#include <utility>
#include <vector>

namespace microide::plugin::discovery_interop {

std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots(
    const std::filesystem::path& current_project_root);

}  // namespace microide::plugin::discovery_interop
