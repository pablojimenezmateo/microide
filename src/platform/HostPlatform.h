#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::platform {

enum class HostPlatform {
  Linux,
  MacOS,
  Windows,
};

HostPlatform CurrentHostPlatform();
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

// Set the process's CHARACTER locale (LC_CTYPE only) to the user's, or to
// C.UTF-8 when the user's is not UTF-8 (a bare `C`/`POSIX` environment). The app
// is UTF-8 throughout, and Xlib converts text through the C library's locale: in
// the default "C" locale SDL's X11 title setter cannot convert a non-ASCII window
// title and leaves `_NET_WM_NAME` (what taskbars and Alt+Tab show) at its old
// value. Numbers stay locale-free (util/Parse.h, JsonValue use from_chars /
// to_chars), and LC_NUMERIC is not touched. Call once at startup, before any
// thread starts (setlocale is not thread-safe). Returns the LC_CTYPE now in
// effect. No-op on Windows.
std::string UseUtf8CharacterLocale();

// Test seam: overrides the platform reported by CurrentHostPlatform(). Always
// compiled (the override defaults to unset, so production behavior is native).
void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform);

// The current working directory of local process `pid` (/proc/<pid>/cwd), or
// empty when it is unknown: no such process, not permitted, or not Linux.
std::filesystem::path ProcessWorkingDirectory(int pid);

}  // namespace microide::platform
