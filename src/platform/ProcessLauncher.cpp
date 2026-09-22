#include "platform/ProcessLauncher.h"

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

  SubprocessResult Run(std::vector<std::string> argv,
                                 SubprocessOptions options) const override {
    return RunSubprocess(argv, options);
  }

  bool is_local() const override { return true; }

  std::string_view description() const override { return "local"; }
};

}  // namespace

const ProcessLauncher& LocalProcessLauncher() {
  static const LocalLauncher launcher;
  return launcher;
}

}  // namespace microide::platform
