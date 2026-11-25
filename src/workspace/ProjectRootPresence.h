#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "workspace/services/NotificationService.h"

namespace microide::workspace {

inline constexpr std::string_view kProjectRootMissingNotificationKey = "project.root.missing";

inline bool PublishProjectRootPresence(NotificationService& notifications, const std::filesystem::path& root,
                                       std::uint64_t now_ms) {
  if (root.empty()) {
    return notifications.DismissKey(kProjectRootMissingNotificationKey);
  }
  std::error_code error;
  const bool present = std::filesystem::is_directory(root, error) || (error && error != std::errc::no_such_file_or_directory);
  if (present) {
    return notifications.DismissKey(kProjectRootMissingNotificationKey);
  }
  return notifications.Show(
      NotificationService::Request{
          .tone = NotificationService::Tone::Warning,
          .key = std::string(kProjectRootMissingNotificationKey),
          .message = "The project folder " + root.string() +
                     " no longer exists (moved or deleted). Unsaved edits are kept; saving one "
                     "recreates its folder. Close the project, or put the folder back.",
          .sticky = true,
      },
      now_ms);
}

}
