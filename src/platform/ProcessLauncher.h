#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Subprocess.h"

namespace microide::platform {

// WHERE a spawned process runs.
//
// The editor spawns a lot of children — git, ripgrep, formatters, language servers,
// debug adapters, plugin tools, the terminal's shell — from eight sites in four
// directories, each of which reached `RunSubprocess` (or `AsyncSubprocess`,
// or `fork`/`exec`) directly. That is fine while every process runs on the machine
// the window is on, and it is the whole problem once a project's files live somewhere
// else: a session that runs half its tools locally and half remotely looks right and
// runs the wrong `make`.
//
// So locality is OWNED, not chosen per call. A project holds a launcher — the local
// one today, a transport-prefixed one for a remote project — and every spawn that
// belongs to a project goes through the launcher of THAT project. The spawns that
// must never follow the project take `LocalProcessLauncher()` explicitly and say why;
// `xdg-open` is the standing example, because opening a file manager on the build
// server is always wrong.
//
// The local payoff arrives before any of that: `git` becomes testable against a
// scripted launcher instead of whatever `git` the test machine happens to have, which
// is exactly the dependence dev-docs/project/validation-traps.md keeps finding.
class ProcessLauncher {
 public:
  virtual ~ProcessLauncher() = default;

  // Rewrite `argv` into the argv that is actually exec'd. The local launcher returns
  // it unchanged. This is also what an asynchronous or pty-based spawn uses: those
  // keep their own plumbing, and only the argv and the working directory are the
  // launcher's business.
  virtual std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const = 0;

  // Rewrite a working directory into one meaningful on the machine that runs the
  // process. Local: unchanged.
  virtual std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const = 0;
  // The inverse: a path a process on that machine reported (a language server's
  // definition, a debugger's stack frame) as the editor's path for the same file.
  // Local: unchanged. A path the mapping does not cover comes back unchanged too,
  // and is then simply a path outside the project.
  virtual std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const = 0;

  // Run to completion. Every synchronous spawn in the tree goes through here.
  //
  // `options.cwd` is a path on the EDITOR's side of the project -- the project root
  // or a directory under it, which for a remote project is the mirror -- and the
  // launcher maps it, exactly as ResolveWorkingDirectory would. Callers never
  // pre-resolve: half of them did and half did not, which is a contract nobody can
  // implement a remote launcher against. Likewise a caller names its tree through
  // the working directory, never as a path in `argv`, because a launcher maps the
  // one and cannot see the other (git used to pass its root as `-C <root>`).
  virtual SubprocessResult Run(std::vector<std::string> argv,
                                         SubprocessOptions options) const = 0;

  // False for a launcher that runs the process on another machine. Callers use it to
  // refuse a thing that only makes sense locally rather than doing it in the wrong
  // place; they do not use it to branch on transport.
  virtual bool is_local() const = 0;

  // Short label for diagnostics and terminal tab titles ("local", "ssh build-host").
  virtual std::string_view description() const = 0;
};

// The process-wide local launcher. Reached directly only by spawns that must never
// follow the project, and used as the default for everything until a project says
// otherwise.
const ProcessLauncher& LocalProcessLauncher();

// Replace every `${workspaceFolder}` in `argv` with `project_root` as the machine
// that runs the process sees it (`launcher.ResolveWorkingDirectory`), as VS Code's
// launch and task configurations do. A path the editor computes and splices into a
// command is the MIRROR's path in a remote project, and a launcher cannot recognise
// it inside argv (Run's contract); a placeholder resolved per locality can be
// (TD-2026-10-07-322). An argv with no `${` is returned untouched.
std::vector<std::string> ExpandWorkspaceFolder(std::vector<std::string> argv,
                                               const std::filesystem::path& project_root,
                                               const ProcessLauncher& launcher);

}  // namespace microide::platform
