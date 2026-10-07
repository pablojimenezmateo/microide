#pragma once

#include "platform/ProcessLauncher.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

// A `platform::ProcessLauncher` that runs nothing and answers from a script.
//
// This is the seam G2 was built for, and its local payoff: a git-backed code path
// can be driven with no `git` binary, no repository on disk, and no dependence on
// whichever git the machine happens to have. It is shared rather than file-local
// because the interesting cases are the ones REAL git cannot produce on demand —
// git absent (exit 127), git killed by the timeout, output truncated at the
// capture ceiling, a porcelain record cut in half — and those are exactly the
// paths that collapse silently when nobody tests them.
//
// It is also where a remote launcher will plug in: `is_local()` is scriptable, so
// a test can assert that a spawn which must never follow the project refuses to
// run on one that would.
class ScriptedProcessLauncher final : public platform::ProcessLauncher {
 public:
  struct Response {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    // Captured output hit the per-stream ceiling and the child was torn down, so
    // `stdout_text` is a PREFIX of what the command would have produced. A caller
    // that parses it without checking this reports partial data as complete.
    bool truncated = false;
  };

  // Every invocation, in order, as the launcher was asked to run it. Assertions
  // about the argv a caller BUILDS are otherwise only observable by running the
  // real program and inspecting its side effects.
  mutable std::vector<std::vector<std::string>> runs;
  // The working directory each `runs` entry was asked for, in the same order.
  mutable std::vector<std::filesystem::path> run_cwds;

  // Every argv resolved through this launcher. A long-lived spawn (a language
  // server, a debug adapter) does not go through `Run`: it resolves its argv here
  // and starts the process itself, so this is how a test sees which launcher it
  // asked. The argv is passed through unchanged, so the process really starts.
  mutable std::vector<std::vector<std::string>> resolved_argvs;

  // Answer every invocation with this unless a queued response is waiting.
  Response standing_response;

  // Answers consumed in order, one per invocation, before `standing_response`.
  // For a caller that issues several commands and whose behaviour depends on
  // which one failed.
  mutable std::vector<Response> queued_responses;

  ScriptedProcessLauncher() = default;
  explicit ScriptedProcessLauncher(Response response)
      : standing_response(std::move(response)) {}

  // The launcher that is not there: `execvp` reports ENOENT as exit 127, so this
  // is what every git call looks like on a machine with no git installed.
  static ScriptedProcessLauncher MissingProgram() {
    return ScriptedProcessLauncher(Response{.exit_code = 127,
                                            .stderr_text = "execvp: No such file or directory"});
  }

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    resolved_argvs.push_back(argv);
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return cwd;
  }
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override {
    return host_path;
  }
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override {
    runs.push_back(std::move(argv));
    run_cwds.push_back(std::move(options.cwd));
    Response response = standing_response;
    if (!queued_responses.empty()) {
      response = queued_responses.front();
      queued_responses.erase(queued_responses.begin());
    }
    return platform::SubprocessResult{
        .exit_code = response.exit_code,
        .stdout_text = std::move(response.stdout_text),
        .stderr_text = std::move(response.stderr_text),
        .timed_out = response.timed_out,
        .truncated = response.truncated,
    };
  }
  bool is_local() const override { return local; }
  std::string_view description() const override { return "scripted"; }

  // Scriptable so a test can drive the "this spawn must never follow the project"
  // refusals without a real remote.
  bool local = true;

  // Did any invocation carry `word` anywhere in its argv?
  [[nodiscard]] bool AnyRunContains(std::string_view word) const {
    for (const std::vector<std::string>& argv : runs) {
      for (const std::string& entry : argv) {
        if (entry == word) {
          return true;
        }
      }
    }
    return false;
  }
};

}  // namespace microide::tests
