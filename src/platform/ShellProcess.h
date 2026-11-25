#pragma once

#include <string>
#include <vector>

namespace microide::platform {

std::string DefaultShellPath();

std::string ShellProgramName(const std::string& shell_path);

std::vector<std::string> BuildTerminalArgv(const std::vector<std::string>& shell,
                                           const std::string& command);

void RequestTerminalChildShutdown(int child_pid);

}
