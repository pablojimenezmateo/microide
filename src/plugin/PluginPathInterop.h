#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace microide::plugin::path_interop {

std::string Basename(const std::filesystem::path& path);
std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path);

std::optional<std::filesystem::path> ContainPath(
    std::span<const std::filesystem::path> allowed_roots,
    const std::filesystem::path& requested);

}
