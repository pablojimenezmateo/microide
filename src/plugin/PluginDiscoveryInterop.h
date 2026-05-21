#pragma once

#include <filesystem>
#include <vector>

namespace microide::plugin::discovery_interop {

std::vector<std::filesystem::path> DiscoverPluginRoots();

}  // namespace microide::plugin::discovery_interop
