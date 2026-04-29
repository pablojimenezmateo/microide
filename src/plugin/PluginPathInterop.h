#pragma once

#include <filesystem>
#include <string>

namespace microide::plugin::path_interop {

std::string Basename(const std::filesystem::path& path);
std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path);

}  // namespace microide::plugin::path_interop
