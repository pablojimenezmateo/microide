#pragma once

#include <filesystem>
#include <string>
#include <string_view>

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

}  // namespace microide::platform
