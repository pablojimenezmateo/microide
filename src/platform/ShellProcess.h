#pragma once

#include <string>
#include <vector>

namespace microide::platform {

// Path to the interactive shell to launch for a terminal session. Honors $SHELL
// (or %COMSPEC% on Windows) and falls back to a sane platform default.
std::string DefaultShellPath();

// Basename of a shell path, e.g. "/usr/bin/zsh" -> "zsh". Used for argv[0].
std::string ShellProgramName(const std::string& shell_path);

// Full child argv for a terminal session, from the configured `terminal.shell`
// (already split into words) and an optional command to run instead of an
// interactive shell. Empty `shell` means the platform default.
//
// This is the one place that decides how a command is attached to a shell,
// because getting it wrong is silent rather than loud: appending a flag to a
// value that is not a shell makes the flag mean something else. `ssh build-host`
// plus `-lc make` is `ssh -l c` — log in as user "c" and run `make` remotely,
// which succeeds at the wrong thing. It is the same bug as the `-i` that this
// field's multi-word form exists to fix, one path over.
std::vector<std::string> BuildTerminalArgv(const std::vector<std::string>& shell,
                                           const std::string& command);

// Terminate a terminal child process (and its process group) with a graceful
// escalation: SIGHUP, then SIGTERM, then SIGKILL, waiting a short bounded grace
// between each. No-op for non-positive pids and on non-POSIX platforms.
void RequestTerminalChildShutdown(int child_pid);

}  // namespace microide::platform
