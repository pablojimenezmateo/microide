#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "workspace/services/NotificationService.h"

namespace microide::workspace {

// The sticky row saying the active project's folder is gone: deleted or moved
// while it was open. Nothing said so -- the tree emptied, a file opened as an
// empty "new" buffer, and a save quietly recreated the folder -- so a user could
// edit for a while in a project that no longer existed where they thought.
// One key: only the active project's is shown, and a switch re-evaluates it.
inline constexpr std::string_view kProjectRootMissingNotificationKey = "project.root.missing";

// Raise or clear that row for `root` (a stat). True when the rows changed, so the
// caller repaints. Called where the project's state is re-read: each applied
// watcher batch and each project switch.
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

}  // namespace microide::workspace
