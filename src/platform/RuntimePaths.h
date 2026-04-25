#pragma once

#include <filesystem>
#include <string_view>

namespace microide::platform {

std::filesystem::path ResolveBundledAssetDirectory();
std::filesystem::path ResolveBundledAssetPath(std::string_view relative_path);

}  // namespace microide::platform
