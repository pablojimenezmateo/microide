#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "platform/HostPlatform.h"
#include "platform/Subprocess.h"
#include "platform/SubprocessSandbox.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
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
#if defined(_WIN32)
  const std::vector<std::string> cwd_command{"cmd", "/c", "cd"};
#else
  const std::vector<std::string> cwd_command{"pwd"};
#endif
  const auto pwd_result = RunSubprocess(cwd_command, SubprocessOptions{
                                                         .cwd = temp_dir.path(),
                                                         .stdin_text = {},
                                                         .environment_overrides = {},
                                                         .capture_stdout = true,
                                                         .capture_stderr = true,
                                                         .silence_stderr = false,
                                                     });
  Expect(pwd_result.exit_code == 0, "subprocess pwd fixture should exit successfully");
  const std::string expected_cwd = temp_dir.path().lexically_normal().string();
  const std::string cwd_message =
      "subprocess execution should honor the requested working directory; expected '" +
      expected_cwd + "' in '" + pwd_result.stdout_text + "'";
  Expect(pwd_result.stdout_text.find(expected_cwd) != std::string::npos, cwd_message);

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

void TestAsyncSubprocessReadTimeoutDoesNotBlockConcurrentWrite() {
  microide::platform::AsyncSubprocess process;
  Expect(process.Start({"cat"}),
         "async subprocess concurrency fixture should start a cat process");

  std::atomic<bool> stop_reader{false};
  std::atomic<bool> reader_running{false};
  std::mutex reader_output_mutex;
  std::string reader_output;
  std::thread reader([&]() {
    while (!stop_reader.load(std::memory_order_acquire)) {
      reader_running.store(true, std::memory_order_release);
      const auto chunk = process.Read(4096, 50);
      if (!chunk.has_value()) {
        return;
      }
      if (!chunk->empty()) {
        std::lock_guard<std::mutex> lock(reader_output_mutex);
        reader_output += *chunk;
      }
    }
  });

  // Wait for the reader to actually enter its loop (so a Read is in flight)
  // rather than guessing with a fixed sleep, then time a concurrent write.
  const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!reader_running.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < reader_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(reader_running.load(std::memory_order_acquire),
         "async subprocess concurrency fixture should start its background reader");
  const auto start = std::chrono::steady_clock::now();
  Expect(process.Write("ping\n"),
         "async subprocess concurrency fixture should accept stdin while reads are pending");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  stop_reader.store(true, std::memory_order_release);
  if (reader.joinable()) {
    reader.join();
  }

  std::string echoed;
  {
    std::lock_guard<std::mutex> lock(reader_output_mutex);
    echoed = reader_output;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() <= deadline) {
    const auto chunk = process.Read(4096, 50);
    if (!chunk.has_value()) {
      break;
    }
    if (chunk->empty()) {
      continue;
    }
    echoed += *chunk;
    if (echoed.find("ping\n") != std::string::npos) {
      break;
    }
  }
  process.Shutdown(0);

  Expect(elapsed.count() < 500,
         "async subprocess writes should not stall behind read timeouts on another thread");
  Expect(echoed.find("ping\n") != std::string::npos,
         "async subprocess concurrency fixture should echo the written payload");
}

// Regression: writing to a pipe whose read end has closed must return EPIPE rather
// than raise SIGPIPE and terminate the process. Simply reaching the assertions
// proves the signal was ignored (otherwise the test binary would have been killed).
void TestIgnoreBrokenPipeSignalPreventsCrash() {
  microide::platform::IgnoreBrokenPipeSignal();
  int fds[2] = {-1, -1};
  Expect(::pipe(fds) == 0, "pipe() should succeed");
  ::close(fds[0]);  // close the read end so the next write breaks the pipe
  errno = 0;
  const char byte = 'x';
  const ssize_t written = ::write(fds[1], &byte, 1);
  const int saved_errno = errno;
  ::close(fds[1]);
  Expect(written == -1, "write to a broken pipe should fail");
  Expect(saved_errno == EPIPE, "write to a broken pipe should report EPIPE, not crash");
}

// Regression: a child that echoes a payload larger than the kernel pipe buffer
// would deadlock if the parent wrote all of stdin before draining stdout — the
// child blocks on write(stdout) while the parent blocks on write(stdin). The
// pump must interleave the two directions. A hang here (caught by the ctest
// timeout) is the failure mode; completing with the echoed payload is the pass.
void TestSubprocessLargeStdinDoesNotDeadlock() {
  microide::platform::IgnoreBrokenPipeSignal();
  // 4 MiB dwarfs the ~64 KiB default pipe buffer on Linux, so both directions
  // must make progress concurrently for `cat` to finish.
  std::string payload;
  payload.reserve(4u * 1024u * 1024u);
  for (std::size_t i = 0; i < 4u * 1024u * 1024u; ++i) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }

  const auto result = RunSubprocess({"cat"}, SubprocessOptions{
                                                 .cwd = {},
                                                 .stdin_text = payload,
                                                 .environment_overrides = {},
                                                 .capture_stdout = true,
                                                 .capture_stderr = true,
                                                 .silence_stderr = false,
                                             });
  Expect(result.exit_code == 0, "large-stdin cat fixture should exit successfully");
  Expect(result.stdout_text.size() == payload.size(),
         "large-stdin cat should echo the entire payload without truncation");
  Expect(result.stdout_text == payload,
         "large-stdin cat should echo the payload byte-for-byte");
  Expect(result.stderr_text.empty(), "large-stdin cat should not write to stderr");
}

// A formatter that hangs (or runs pathologically long) must not freeze the
// synchronous save path forever. A finite timeout_ms kills the child and reports
// timed_out with a non-zero exit code.
void TestSubprocessTimeoutKillsHungChild() {
  const auto start = std::chrono::steady_clock::now();
  const auto result = RunSubprocess({"sleep", "30"}, SubprocessOptions{
                                                         .cwd = {},
                                                         .stdin_text = {},
                                                         .environment_overrides = {},
                                                         .capture_stdout = true,
                                                         .capture_stderr = true,
                                                         .silence_stderr = false,
                                                         .timeout_ms = 200,
                                                     });
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  Expect(result.timed_out, "a child that outlives timeout_ms should report timed_out");
  Expect(!result.success(), "a timed-out child should not report success");
  Expect(elapsed.count() < 5000,
         "timeout should return promptly after the deadline, not wait for the child to exit");
}

// A child that CLOSES its stdout/stderr and then hangs must still be bounded by
// timeout_ms. Draining the pipes returns as soon as they close, so the timeout has
// to be enforced against process exit, not just against the pipes going quiet.
void TestSubprocessTimeoutKillsChildThatClosedStdioThenHung() {
  const auto start = std::chrono::steady_clock::now();
  // Close fds 1 and 2, then sleep far past the timeout with no output at all.
  const auto result = RunSubprocess({"sh", "-c", "exec 1>&- 2>&-; sleep 30"},
                                    SubprocessOptions{
                                        .cwd = {},
                                        .stdin_text = {},
                                        .environment_overrides = {},
                                        .capture_stdout = true,
                                        .capture_stderr = true,
                                        .silence_stderr = false,
                                        .timeout_ms = 200,
                                    });
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  Expect(result.timed_out,
         "a child that closes stdio and then hangs must still hit the timeout");
  Expect(elapsed.count() < 5000,
         "the timeout must fire promptly, not block on the closed-stdio child forever");
}

// A child that floods stdout without end must not grow the capture buffer until
// the host OOMs. The capture ceiling truncates the stream, marks the result
// truncated, and tears the child down promptly instead of reading forever.
void TestSubprocessCaptureCapTruncatesFirehose() {
  microide::platform::IgnoreBrokenPipeSignal();
  const auto start = std::chrono::steady_clock::now();
  // `yes` streams "y\n" indefinitely; without a cap this call would never return
  // and would exhaust memory.
  const auto result = RunSubprocess({"yes"}, SubprocessOptions{
                                                 .cwd = {},
                                                 .stdin_text = {},
                                                 .environment_overrides = {},
                                                 .capture_stdout = true,
                                                 .capture_stderr = true,
                                                 .silence_stderr = false,
                                             });
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  Expect(result.truncated, "an endless firehose child should report truncated capture");
  // Bounded at the ceiling (128 MiB): well above 64 MiB, never gigabytes.
  Expect(result.stdout_text.size() >= 64ull * 1024 * 1024,
         "capture should fill up to the ceiling before truncating");
  Expect(result.stdout_text.size() <= 129ull * 1024 * 1024,
         "capture must not exceed the ceiling");
  Expect(elapsed.count() < 15000, "a firehose child should be torn down promptly, not read forever");
}

// A finite timeout must not falsely trip on a command that finishes well within
// it: the bound only catches genuine hangs.
void TestSubprocessTimeoutDoesNotTripFastCommand() {
  const auto result = RunSubprocess({"cat"}, SubprocessOptions{
                                                 .cwd = {},
                                                 .stdin_text = "fast payload\n",
                                                 .environment_overrides = {},
                                                 .capture_stdout = true,
                                                 .capture_stderr = true,
                                                 .silence_stderr = false,
                                                 .timeout_ms = 5000,
                                             });
  Expect(!result.timed_out, "a fast command should not be reported as timed out");
  Expect(result.exit_code == 0, "a fast command under a generous timeout should still succeed");
  Expect(result.stdout_text == "fast payload\n",
         "a timed run should still capture stdout when it completes in time");
}
#endif

// The probe is parent-side and read-only, so it is safe to call from the test process. We assert
// internal consistency rather than concrete availability: CI kernels vary (Landlock/seccomp may be
// absent), so the only invariants we can guarantee are that a layer can never be reported "runtime
// available" without also being "compiled in", and that the ABI/flag pair agrees. This also pins the
// must-not-confine-the-host contract: calling the probe here would break every later test in this
// binary if it accidentally restricted the process.
void TestSandboxProbeReportsConsistentSupport() {
  const microide::platform::SandboxSupport support = microide::platform::ProbeSandboxSupport();

  Expect(!support.landlock_runtime_available || support.compiled_with_landlock,
         "landlock cannot be runtime-available unless it was compiled in");
  Expect(!support.seccomp_runtime_available || support.compiled_with_seccomp,
         "seccomp cannot be runtime-available unless it was compiled in");
  Expect((support.landlock_abi >= 1) == support.landlock_runtime_available,
         "landlock_abi >= 1 must agree with landlock_runtime_available");
  Expect(support.landlock_abi >= 0, "landlock_abi must never be negative");

  const bool expected_active =
      support.landlock_runtime_available &&
      (support.compiled_with_seccomp ? support.seccomp_runtime_available : true);
  Expect(support.fully_active() == expected_active,
         "fully_active() must reflect the per-layer availability flags");

  // A second probe must agree: the query has no side effects on the host process.
  const microide::platform::SandboxSupport again = microide::platform::ProbeSandboxSupport();
  Expect(again.landlock_runtime_available == support.landlock_runtime_available &&
             again.landlock_abi == support.landlock_abi &&
             again.seccomp_runtime_available == support.seccomp_runtime_available,
         "repeated sandbox probes must be stable (no host-process side effects)");
}

}  // namespace

void RegisterSubprocessTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Subprocess/SandboxProbeReportsConsistentSupport",
          TestSandboxProbeReportsConsistentSupport);
  AddTest(tests, "Subprocess/CapturesStdoutAndStdin", TestSubprocessCapturesStdoutAndStdin);
  AddTest(tests, "Subprocess/CapturesStderrAndCwd", TestSubprocessCapturesStderrAndCwd);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "Subprocess/AppliesEnvironmentOverrides",
          TestSubprocessAppliesEnvironmentOverrides);
  AddTest(tests, "Subprocess/WithoutExplicitStdinDoesNotInheritParentStdin",
          TestSubprocessWithoutExplicitStdinDoesNotInheritParentStdin);
  AddTest(tests, "Subprocess/AsyncReadTimeoutDoesNotBlockConcurrentWrite",
          TestAsyncSubprocessReadTimeoutDoesNotBlockConcurrentWrite);
  AddTest(tests, "Subprocess/IgnoreBrokenPipeSignalPreventsCrash",
          TestIgnoreBrokenPipeSignalPreventsCrash);
  AddTest(tests, "Subprocess/LargeStdinDoesNotDeadlock",
          TestSubprocessLargeStdinDoesNotDeadlock);
  AddTest(tests, "Subprocess/TimeoutKillsHungChild", TestSubprocessTimeoutKillsHungChild);
  AddTest(tests, "Subprocess/TimeoutKillsChildThatClosedStdioThenHung",
          TestSubprocessTimeoutKillsChildThatClosedStdioThenHung);
  AddTest(tests, "Subprocess/CaptureCapTruncatesFirehose",
          TestSubprocessCaptureCapTruncatesFirehose);
  AddTest(tests, "Subprocess/TimeoutDoesNotTripFastCommand",
          TestSubprocessTimeoutDoesNotTripFastCommand);
#endif
}

}  // namespace microide::tests
