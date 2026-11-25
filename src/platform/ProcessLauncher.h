#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Subprocess.h"

namespace microide::platform {

class AsyncSubprocess;

class ProcessLauncher {
 public:
  virtual ~ProcessLauncher() = default;

  virtual std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const = 0;

  virtual std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const = 0;
  virtual std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const = 0;
  virtual std::string DisplayPathOf(const std::filesystem::path& local_path) const {
    (void)local_path;
    return {};
  }

  virtual SubprocessResult Run(std::vector<std::string> argv,
                                         SubprocessOptions options) const = 0;

  virtual bool StartAsync(AsyncSubprocess& process, const std::vector<std::string>& argv,
                          const std::filesystem::path& cwd,
                          const SubprocessSandbox& sandbox) const;

  virtual bool is_local() const = 0;

  virtual std::string_view description() const = 0;
};

const ProcessLauncher& LocalProcessLauncher();

inline const ProcessLauncher& LauncherOrLocal(const ProcessLauncher* launcher) {
  return launcher != nullptr ? *launcher : LocalProcessLauncher();
}

std::vector<std::string> ExpandWorkspaceFolder(std::vector<std::string> argv,
                                               const std::filesystem::path& project_root,
                                               const ProcessLauncher& launcher);

std::vector<std::string> FirstAvailableCommand(
    const std::vector<std::vector<std::string>>& candidates, std::string_view install_hint);

}
