#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::platform {

struct HostIntegrationResult {
  bool ok = false;
  std::string error_message;
};

HostIntegrationResult OpenUrl(std::string_view url);

// Opens the OS file manager at the given directory (Linux: `xdg-open`).
HostIntegrationResult OpenPathInFileManager(const std::filesystem::path& directory);

}  // namespace microide::platform
