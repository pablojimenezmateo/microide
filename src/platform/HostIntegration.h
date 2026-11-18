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

// True when OpenUrl would hand this URL to the desktop opener: a fully-qualified
// http/https/mailto URL of a sane length. Split out so a caller on the UI thread can
// reject a bad URL synchronously and still dispatch the spawn itself elsewhere.
bool IsOpenableExternalUrl(std::string_view url);

// BLOCKS until the opener exits or the timeout fires. `xdg-open` normally forks and
// returns at once, but its generic $BROWSER fallback does not, so this must not be
// called on the shell thread -- WorkspaceShell posts it to the background executor,
// the same way the file-manager reveal does.
HostIntegrationResult OpenUrl(std::string_view url);

// Opens the OS file manager at the given directory (Linux: `xdg-open`). Blocks for
// the same reason OpenUrl does, with the same rule about the calling thread.
HostIntegrationResult OpenPathInFileManager(const std::filesystem::path& directory);

// The command ShowDesktopNotification runs, split out so the text it hands the
// notification daemon is tested without one. `summary` and `body` come from a
// terminal program, so they are untrusted: control bytes become spaces, both are
// cut to a byte budget on a code-point boundary, and the body's markup
// characters are escaped (the daemon renders `<b>` and friends in a body).
std::vector<std::string> DesktopNotificationCommand(std::string_view summary, std::string_view body);

// Posts a desktop notification through the freedesktop notification service
// (`notify-send`). Blocks until the tool exits or its timeout fires, with the same
// rule about the calling thread as OpenUrl. A desktop without the tool or without
// a notification daemon fails here and nothing else happens.
HostIntegrationResult ShowDesktopNotification(std::string_view summary, std::string_view body);

}  // namespace microide::platform
