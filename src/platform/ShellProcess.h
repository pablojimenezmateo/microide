#pragma once

#include <string>

namespace microide::platform {

// Path to the interactive shell to launch for a terminal session. Honors $SHELL
// (or %COMSPEC% on Windows) and falls back to a sane platform default.
std::string DefaultShellPath();

// Basename of a shell path, e.g. "/usr/bin/zsh" -> "zsh". Used for argv[0].
std::string ShellProgramName(const std::string& shell_path);

// Terminate a terminal child process (and its process group) with a graceful
// escalation: SIGHUP, then SIGTERM, then SIGKILL, waiting a short bounded grace
// between each. No-op for non-positive pids and on non-POSIX platforms.
void RequestTerminalChildShutdown(int child_pid);

}  // namespace microide::platform
