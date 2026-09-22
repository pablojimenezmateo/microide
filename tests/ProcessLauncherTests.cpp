#include "TestSupport.h"

#include "platform/ProcessLauncher.h"
#include "project/GitRepository.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::ProcessLauncher;
using microide::platform::SubprocessOptions;
using microide::platform::SubprocessResult;

// The point of the launcher seam, demonstrated: a git-backed code path can be driven
// without a `git` binary at all. Before it, every git test needed whatever git the
// test machine happened to have, with whatever config and version — the exact
// dependence dev-docs/project/validation-traps.md keeps finding.
class ScriptedLauncher final : public ProcessLauncher {
 public:
  mutable std::vector<std::vector<std::string>> runs;
  std::string stdout_text;
  int exit_code = 0;

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return cwd;
  }
  SubprocessResult Run(std::vector<std::string> argv, SubprocessOptions) const override {
    runs.push_back(argv);
    return SubprocessResult{.exit_code = exit_code, .stdout_text = stdout_text};
  }
  bool is_local() const override { return true; }
  std::string_view description() const override { return "scripted"; }
};

void TestScriptedLauncherDrivesGitWithoutAGitBinary() {
  ScriptedLauncher launcher;
  launcher.stdout_text = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef\n";
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto result = repo.Execute({"rev-parse", "--verify", "HEAD"});
  Expect(result.success(), "the scripted launcher's exit code is what the repository reports");
  Expect(result.output.find("deadbeef") != std::string::npos,
         "the scripted launcher's stdout is what the repository reports");
  Expect(launcher.runs.size() == 1, "exactly one git invocation was issued");

  // The argv the repository builds is a contract of its own: `--no-optional-locks`
  // keeps a read command off .git/index.lock, and `--literal-pathspecs` stops a file
  // whose name begins with git pathspec magic from being interpreted as a pattern.
  // Both were previously unobservable without running git and inspecting side effects.
  const std::vector<std::string>& argv = launcher.runs.front();
  Expect(!argv.empty() && argv.front() == "git", "the program is git");
  const auto has = [&](std::string_view flag) {
    for (const std::string& word : argv) {
      if (word == flag) {
        return true;
      }
    }
    return false;
  };
  Expect(has("--no-optional-locks"),
         "every git command suppresses the optional index refresh");
  Expect(has("--literal-pathspecs"), "every git command forces literal pathspecs");
  Expect(has("-C"), "the repository root is passed with -C rather than a chdir");
  Expect(argv.back() == "HEAD" && argv[argv.size() - 2] == "--verify",
         "the caller's arguments come last, in order");
}

void TestScriptedLauncherReportsFailureThrough() {
  ScriptedLauncher launcher;
  launcher.exit_code = 128;
  launcher.stdout_text = "fatal: not a git repository";
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.ExecuteSucceeds({"status"}),
         "a non-zero scripted exit code must surface as failure");
}

}  // namespace

void RegisterProcessLauncherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProcessLauncher/ScriptedLauncherDrivesGitWithoutAGitBinary",
          TestScriptedLauncherDrivesGitWithoutAGitBinary);
  AddTest(tests, "ProcessLauncher/ScriptedLauncherReportsFailureThrough",
          TestScriptedLauncherReportsFailureThrough);
}

}  // namespace microide::tests
