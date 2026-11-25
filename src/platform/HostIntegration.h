#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::platform {

struct HostIntegrationResult {
  bool ok = false;
  std::string error_message;
};

bool IsOpenableExternalUrl(std::string_view url);

HostIntegrationResult OpenUrl(std::string_view url);

HostIntegrationResult OpenPathInFileManager(const std::filesystem::path& directory);

std::vector<std::string> DesktopNotificationCommand(std::string_view summary, std::string_view body);

HostIntegrationResult ShowDesktopNotification(std::string_view summary, std::string_view body);

}
