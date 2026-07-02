#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::platform {

// Absolute path of the currently running executable (/proc/self/exe on Linux,
// _NSGetExecutablePath on macOS, GetModuleFileNameW on Windows). Empty on
// failure. Used to relaunch microide for a detached-tab window.
std::filesystem::path CurrentExecutablePath();

// Fire-and-forget launch of a fully detached, long-lived process. Returns true
// when the child was launched (NOT its exit status) and never blocks waiting for
// it to exit -- unlike RunSubprocess, which waits. The child is reparented (POSIX:
// double-fork + setsid) so it survives this process and leaves no zombie; its
// stdio is redirected to /dev/null. `argv[0]` is the program to exec.
bool SpawnDetached(const std::vector<std::string>& argv,
                   const std::filesystem::path& cwd = {});

}  // namespace microide::platform
