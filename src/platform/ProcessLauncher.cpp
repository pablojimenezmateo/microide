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

std::vector<std::string> FirstAvailableCommand(
    const std::vector<std::vector<std::string>>& candidates, std::string_view install_hint) {
  if (candidates.size() == 1) {
    return candidates.front();
  }
  // $1 is the hint; the candidates follow, separated by kSeparator. Pass 1 finds
  // the first candidate whose program exists; pass 2 rotates the positional
  // parameters, keeping only that candidate's, and execs them.
  static constexpr std::string_view kSeparator = "--microide-or--";
  static constexpr std::string_view kScript = R"sh(hint=$1; shift
exists() {
  case $1 in
    "~/"*) [ -x "$HOME/${1#"~/"}" ] ;;
    */*) [ -x "$1" ] ;;
    *) command -v "$1" >/dev/null 2>&1 ;;
  esac
}
group=0; chosen=-1; first=1
for arg do
  if [ "$arg" = --microide-or-- ]; then group=$((group + 1)); first=1; continue; fi
  if [ "$first" = 1 ]; then
    first=0
    if [ "$chosen" = -1 ] && exists "$arg"; then chosen=$group; fi
  fi
done
if [ "$chosen" = -1 ]; then printf '%s
' "$hint" >&2; exit 127; fi
n=$#; i=0; group=0; first=1
while [ "$i" -lt "$n" ]; do
  arg=$1; shift; i=$((i + 1))
  if [ "$arg" = --microide-or-- ]; then group=$((group + 1)); first=1; continue; fi
  if [ "$group" = "$chosen" ]; then
    if [ "$first" = 1 ]; then
      case $arg in "~/"*) arg="$HOME/${arg#"~/"}" ;; esac
    fi
    set -- "$@" "$arg"
  fi
  first=0
done
exec "$@"
)sh";
  std::vector<std::string> argv = {"sh", "-c", std::string(kScript), "microide-resolve",
                                   std::string(install_hint)};
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (i > 0) {
      argv.emplace_back(kSeparator);
    }
    argv.insert(argv.end(), candidates[i].begin(), candidates[i].end());
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
