#pragma once

#include <filesystem>
#include <string_view>

namespace microide::platform {

enum class UserDirectoryKind {
  Config,
  State,
  Data,
  Cache,
};

std::filesystem::path ResolveUserHomeDirectory();
std::filesystem::path ResolveUserDirectory(UserDirectoryKind kind);
std::filesystem::path ResolveAppDirectory(UserDirectoryKind kind, std::string_view app_name);

}  // namespace microide::platform
