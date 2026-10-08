#include "platform/ProcessLauncher.h"

#include "platform/AsyncSubprocess.h"

#include <string_view>
#include <utility>

namespace microide::platform {

namespace {

class LocalLauncher final : public ProcessLauncher {
 public:
  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return argv;
  }

  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return cwd;
  }

  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override {
    return host_path;
  }

  SubprocessResult Run(std::vector<std::string> argv,
                                 SubprocessOptions options) const override {
    return RunSubprocess(argv, options);
  }

  bool is_local() const override { return true; }

  std::string_view description() const override { return "local"; }
};

}  // namespace

std::vector<std::string> ExpandWorkspaceFolder(std::vector<std::string> argv,
                                               const std::filesystem::path& project_root,
                                               const ProcessLauncher& launcher) {
  constexpr std::string_view kPlaceholder = "${workspaceFolder}";
  std::string host_root;
  for (std::string& arg : argv) {
    for (std::size_t at = arg.find(kPlaceholder); at != std::string::npos;
         at = arg.find(kPlaceholder, at + host_root.size())) {
      if (host_root.empty()) {
        host_root = launcher.ResolveWorkingDirectory(project_root).string();
      }
      arg.replace(at, kPlaceholder.size(), host_root);
    }
  }
  return argv;
}

bool ProcessLauncher::StartAsync(AsyncSubprocess& process, const std::vector<std::string>& argv,
                                 const std::filesystem::path& cwd,
                                 const SubprocessSandbox& sandbox) const {
  return process.Start(ResolveArgv(argv), ResolveWorkingDirectory(cwd).string(), sandbox);
}

const ProcessLauncher& LocalProcessLauncher() {
  static const LocalLauncher launcher;
  return launcher;
}

}  // namespace microide::platform
