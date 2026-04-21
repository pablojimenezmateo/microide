#include "TestSupport.h"

#include "platform/Subprocess.h"

#include <optional>
#include <string>

namespace microide::tests {
namespace {

using microide::platform::RunSubprocess;
using microide::platform::SubprocessEnvironmentOverride;
using microide::platform::SubprocessOptions;

void TestSubprocessCapturesStdoutAndStdin() {
  const auto result = RunSubprocess({"cat"}, SubprocessOptions{
                                                 .cwd = {},
                                                 .stdin_text = "stdin payload\n",
                                                 .environment_overrides = {},
                                                 .capture_stdout = true,
                                                 .capture_stderr = true,
                                                 .silence_stderr = false,
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
                                                     .stdin_text = {},
                                                     .environment_overrides = {},
                                                     .capture_stdout = true,
                                                     .capture_stderr = true,
                                                     .silence_stderr = false,
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
                                           SubprocessOptions{
                                               .cwd = {},
                                               .stdin_text = {},
                                               .environment_overrides = {},
                                               .capture_stdout = true,
                                               .capture_stderr = false,
                                               .silence_stderr = true,
                                           });
  Expect(silent_result.exit_code != 0,
         "silenced stderr subprocess fixture should still preserve the command exit code");
  Expect(silent_result.stderr_text.empty(),
         "silenced subprocess execution should discard stderr output");
}

#if defined(__unix__) || defined(__APPLE__)
void TestSubprocessAppliesEnvironmentOverrides() {
  ScopedEnvVar scoped_env("MICROIDE_SUBPROCESS_TEST_ENV", "outer");

  const auto override_result = RunSubprocess(
      {"sh", "-c", "printf '%s' \"$MICROIDE_SUBPROCESS_TEST_ENV\""},
      SubprocessOptions{
          .cwd = {},
          .stdin_text = {},
          .environment_overrides =
              {
                  SubprocessEnvironmentOverride{
                      .name = "MICROIDE_SUBPROCESS_TEST_ENV",
                      .value = std::string("inner"),
                  },
              },
          .capture_stdout = true,
          .capture_stderr = true,
          .silence_stderr = false,
      });
  Expect(override_result.exit_code == 0,
         "subprocess env override fixture should exit successfully");
  Expect(override_result.stdout_text == "inner",
         "subprocess execution should override inherited environment variables");

  const auto unset_result = RunSubprocess(
      {"sh", "-c",
       "if [ -n \"${MICROIDE_SUBPROCESS_TEST_ENV+x}\" ]; then printf set; else printf unset; fi"},
      SubprocessOptions{
          .cwd = {},
          .stdin_text = {},
          .environment_overrides =
              {
                  SubprocessEnvironmentOverride{
                      .name = "MICROIDE_SUBPROCESS_TEST_ENV",
                      .value = std::nullopt,
                  },
              },
          .capture_stdout = true,
          .capture_stderr = true,
          .silence_stderr = false,
      });
  Expect(unset_result.exit_code == 0,
         "subprocess env unset fixture should exit successfully");
  Expect(unset_result.stdout_text == "unset",
         "subprocess execution should allow removing inherited environment variables");
}
#endif

}  // namespace

void RegisterSubprocessTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Subprocess/CapturesStdoutAndStdin", TestSubprocessCapturesStdoutAndStdin);
  AddTest(tests, "Subprocess/CapturesStderrAndCwd", TestSubprocessCapturesStderrAndCwd);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "Subprocess/AppliesEnvironmentOverrides",
          TestSubprocessAppliesEnvironmentOverrides);
#endif
}

}  // namespace microide::tests
