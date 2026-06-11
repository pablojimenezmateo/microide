#pragma once

#include <optional>
#include <string_view>

namespace microide::platform {

enum class HostPlatform {
  Linux,
  MacOS,
  Windows,
};

HostPlatform CurrentHostPlatform();
std::string_view HostPlatformName(HostPlatform platform);

// Ignore SIGPIPE process-wide so a write() to a child pipe/PTY whose reader has
// died returns EPIPE instead of terminating the whole process. Must be called once
// at startup before any subprocess/terminal/LSP I/O. Idempotent and no-op on Windows.
void IgnoreBrokenPipeSignal();

#ifdef MICROIDE_TESTING
void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform);
#endif

}  // namespace microide::platform
