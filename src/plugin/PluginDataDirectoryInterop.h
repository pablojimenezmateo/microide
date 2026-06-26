#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace microide::plugin::data_directory_interop {

// Resolve the existing `<root>/<subdirectory>` directories for the given plugin roots.
// Takes roots (not live PluginInstances) so the caller can pass the published,
// UI-thread-owned roots rather than the live `plugins` vector the worker rebuilds.
std::vector<std::filesystem::path> DataDirectories(
    std::string_view subdirectory,
    const std::vector<std::filesystem::path>& plugin_roots);

}  // namespace microide::plugin::data_directory_interop
