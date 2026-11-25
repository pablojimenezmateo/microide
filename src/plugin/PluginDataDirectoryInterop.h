#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace microide::plugin::data_directory_interop {

std::vector<std::filesystem::path> DataDirectories(
    std::string_view subdirectory,
    const std::vector<std::filesystem::path>& plugin_roots);

}
