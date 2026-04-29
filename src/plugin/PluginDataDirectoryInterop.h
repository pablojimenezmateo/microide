#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::data_directory_interop {

std::vector<std::filesystem::path> DataDirectories(
    std::string_view subdirectory,
    const std::vector<runtime_types::PluginInstance>& plugins);

}  // namespace microide::plugin::data_directory_interop
