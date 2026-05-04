#include "TestSupport.h"

#include "platform/Subprocess.h"

#include <optional>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

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
class ScopedStdinRedirect {
 public:
  explicit ScopedStdinRedirect(const std::filesystem::path& path) {
    saved_stdin_ = dup(STDIN_FILENO);
    Expect(saved_stdin_ >= 0, "stdin redirect fixture should duplicate the current stdin");

    redirected_fd_ = open(path.c_str(), O_RDONLY);
    Expect(redirected_fd_ >= 0, "stdin redirect fixture should open the redirected stdin file");
    Expect(dup2(redirected_fd_, STDIN_FILENO) >= 0,
           "stdin redirect fixture should replace the process stdin");
  }

  ~ScopedStdinRedirect() {
    if (saved_stdin_ >= 0) {
      (void)dup2(saved_stdin_, STDIN_FILENO);
      close(saved_stdin_);
    }
    if (redirected_fd_ >= 0) {
      close(redirected_fd_);
    }
  }

  ScopedStdinRedirect(const ScopedStdinRedirect&) = delete;
  ScopedStdinRedirect& operator=(const ScopedStdinRedirect&) = delete;

 private:
  int saved_stdin_ = -1;
  int redirected_fd_ = -1;
};

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

void TestSubprocessWithoutExplicitStdinDoesNotInheritParentStdin() {
  TemporaryDirectory temp_dir;
  const auto redirected_stdin = temp_dir.path() / "ambient-stdin.txt";
  WriteFile(redirected_stdin, "ambient stdin should not leak\n");

  ScopedStdinRedirect redirect(redirected_stdin);
  const auto result = RunSubprocess(
      {"python3", "-c", "import sys; sys.stdout.write(sys.stdin.read())"},
      SubprocessOptions{
          .cwd = {},
          .stdin_text = {},
          .environment_overrides = {},
          .capture_stdout = true,
          .capture_stderr = true,
          .silence_stderr = false,
      });

  Expect(result.exit_code == 0,
         "stdin inheritance regression fixture should exit successfully");
  Expect(result.stdout_text.empty(),
         "subprocesses without explicit stdin should receive EOF instead of inheriting parent stdin");
  Expect(result.stderr_text.empty(),
         "stdin inheritance regression fixture should not emit stderr");
}
#endif

}  // namespace

void RegisterSubprocessTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Subprocess/CapturesStdoutAndStdin", TestSubprocessCapturesStdoutAndStdin);
  AddTest(tests, "Subprocess/CapturesStderrAndCwd", TestSubprocessCapturesStderrAndCwd);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "Subprocess/AppliesEnvironmentOverrides",
          TestSubprocessAppliesEnvironmentOverrides);
  AddTest(tests, "Subprocess/WithoutExplicitStdinDoesNotInheritParentStdin",
          TestSubprocessWithoutExplicitStdinDoesNotInheritParentStdin);
#endif
}

}  // namespace microide::tests
