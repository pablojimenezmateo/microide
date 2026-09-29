#include "TestSupport.h"

#include "ScriptedProcessLauncher.h"

#include "platform/ProcessLauncher.h"
#include "project/GitRepository.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {


void TestScriptedLauncherDrivesGitWithoutAGitBinary() {
  ScriptedProcessLauncher launcher;
  launcher.standing_response.stdout_text = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef\n";
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
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 128;
  launcher.standing_response.stdout_text = "fatal: not a git repository";
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.ExecuteSucceeds({"status"}),
         "a non-zero scripted exit code must surface as failure");
}


// The cases REAL git cannot be made to produce on demand, which is the whole
// reason the seam is worth having. Each one used to be indistinguishable from a
// clean working tree.
void TestGitNotInstalledIsNotACleanWorkingTree() {
  // `execvp` reports ENOENT as exit 127, so this is every git call on a machine
  // with no git installed.
  const ScriptedProcessLauncher launcher = ScriptedProcessLauncher::MissingProgram();
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(!entries.has_value(),
         "git failing to run is NO ANSWER, not an answer of 'nothing changed' — the "
         "conflict review reported 'no conflicts' for exactly this");
}

void TestCleanWorkingTreeIsAnAnswer() {
  // git ran and said nothing, which is what a clean tree looks like. The empty
  // case has to stay distinguishable from the failure above, or the fix traded
  // one wrong answer for another.
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  launcher.standing_response.stdout_text = "";
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(entries.has_value(), "a successful status is an answer");
  Expect(entries->empty(), "and the answer is that the tree is clean");
}

void TestTruncatedStatusIsNotACompleteChangeList() {
  // Output that hit the capture ceiling carries REAL entries — just not all of
  // them. Reporting a prefix as the whole list is the dangerous direction: the
  // user resolves what they are shown and believes they are finished.
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  launcher.standing_response.stdout_text = std::string("UU conflicted.txt") + '\0';
  launcher.standing_response.truncated = true;
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.GetWorkingTreeEntries().has_value(),
         "a truncated status cannot be presented as the complete set of changes");
}

}  // namespace

void RegisterProcessLauncherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProcessLauncher/ScriptedLauncherDrivesGitWithoutAGitBinary",
          TestScriptedLauncherDrivesGitWithoutAGitBinary);
  AddTest(tests, "ProcessLauncher/ScriptedLauncherReportsFailureThrough",
          TestScriptedLauncherReportsFailureThrough);
  AddTest(tests, "ProcessLauncher/GitNotInstalledIsNotACleanWorkingTree",
          TestGitNotInstalledIsNotACleanWorkingTree);
  AddTest(tests, "ProcessLauncher/CleanWorkingTreeIsAnAnswer", TestCleanWorkingTreeIsAnAnswer);
  AddTest(tests, "ProcessLauncher/TruncatedStatusIsNotACompleteChangeList",
          TestTruncatedStatusIsNotACompleteChangeList);
}

}  // namespace microide::tests
