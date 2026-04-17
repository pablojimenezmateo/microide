#include "TestSupport.h"

#include "platform/Subprocess.h"

#include <string>

namespace microide::tests {
namespace {

using microide::platform::RunSubprocess;
using microide::platform::SubprocessOptions;

void TestSubprocessCapturesStdoutAndStdin() {
  const auto result = RunSubprocess({"cat"}, SubprocessOptions{
                                                 .stdin_text = "stdin payload\n",
                                             });
  Expect(result.exit_code == 0, "subprocess cat fixture should exit successfully");
  Expect(result.stdout_text == "stdin payload\n",
         "subprocess execution should capture stdout from stdin-driven commands");
  Expect(result.stderr_text.empty(),
         "subprocess execution should keep stderr empty when nothing is written");
}

void TestSubprocessCapturesStderrAndCwd() {
  TemporaryDirectory temp_dir;
  const auto pwd_result = RunSubprocess({"pwd"}, SubprocessOptions{
                                                     .cwd = temp_dir.path(),
                                                 });
  Expect(pwd_result.exit_code == 0, "subprocess pwd fixture should exit successfully");
  Expect(pwd_result.stdout_text.find(temp_dir.path().lexically_normal().string()) != std::string::npos,
         "subprocess execution should honor the requested working directory");

  const auto stderr_result = RunSubprocess({"git", "definitely-not-a-command"});
  Expect(stderr_result.exit_code != 0,
         "stderr subprocess fixture should fail for an invalid git subcommand");
  Expect(!stderr_result.stderr_text.empty(),
         "subprocess execution should capture stderr output");

  const auto silent_result = RunSubprocess({"git", "definitely-not-a-command"},
                                           SubprocessOptions{.capture_stderr = false,
                                                             .silence_stderr = true});
  Expect(silent_result.exit_code != 0,
         "silenced stderr subprocess fixture should still preserve the command exit code");
  Expect(silent_result.stderr_text.empty(),
         "silenced subprocess execution should discard stderr output");
}

}  // namespace

void RegisterSubprocessTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Subprocess/CapturesStdoutAndStdin", TestSubprocessCapturesStdoutAndStdin);
  AddTest(tests, "Subprocess/CapturesStderrAndCwd", TestSubprocessCapturesStderrAndCwd);
}

}  // namespace microide::tests
