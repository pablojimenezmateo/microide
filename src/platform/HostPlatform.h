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

// True when the host filesystem treats paths case-insensitively (the default on
// Windows and macOS). Honors the test override so path-casing behavior can be
// exercised on a Linux CI host. Correctness note: on a case-sensitive host, `.GIT`
// is a genuinely different directory from `.git`, so callers must NOT case-fold
// there — this gate keeps that distinction.
bool HostPathsAreCaseInsensitive();

// Ignore SIGPIPE process-wide so a write() to a child pipe/PTY whose reader has
// died returns EPIPE instead of terminating the whole process. Must be called once
// at startup before any subprocess/terminal/LSP I/O. Idempotent and no-op on Windows.
void IgnoreBrokenPipeSignal();

// Test seam: overrides the platform reported by CurrentHostPlatform(). Always
// compiled (the override defaults to unset, so production behavior is native).
void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform);

}  // namespace microide::platform
