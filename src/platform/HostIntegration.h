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
HostIntegrationResult RevealPath(const std::filesystem::path& path);

}  // namespace microide::platform
