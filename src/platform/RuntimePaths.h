#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::platform {

std::filesystem::path ResolveBundledAssetDirectoryForBasePath(
    const std::filesystem::path& base_path,
    const std::filesystem::path& explicit_asset_root = {});
std::filesystem::path ResolveBundledAssetDirectory();
std::filesystem::path ResolveBundledAssetPath(std::string_view relative_path);

bool EnsureSecurePrivateDirectory(const std::filesystem::path& dir);

std::filesystem::path ResolveBundledServerBinary(std::string_view arch);
std::filesystem::path ResolveBundledServerBinaryForBasePath(const std::filesystem::path& base,
                                                            std::string_view arch);
std::string ThisMachineArchitecture();
std::string NormalizeArchitecture(std::string_view uname_m);

}
