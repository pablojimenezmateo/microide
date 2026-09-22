#include "TestSupport.h"

#include "platform/ShellProcess.h"
#include "platform/TerminalBackend.h"

#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#endif

namespace microide::tests {
namespace {

#if defined(__unix__) || defined(__APPLE__)

using microide::platform::CreateTerminalBackend;
using microide::platform::TerminalBackendCallbacks;
using microide::platform::TerminalStartRequest;

// TD-2026-07-17-014: a child that stops draining its stdin fills the PTY input
// buffer. The old backend wrote to the (blocking) master directly, so the
// caller — historically the UI thread on paste/keystroke — parked inside
// write() until the child read again (here: never, until the 30s sleep ends),
// freezing the app. Writes now buffer and drain off the reader thread, so
// Write() must return promptly regardless of the child.
void TestTerminalBackendWriteDoesNotBlockOnStuckChild() {
  auto backend = CreateTerminalBackend();
  const auto result = backend->Start(
      TerminalStartRequest{.command = "sleep 30", .rows = 24, .columns = 80},
      TerminalBackendCallbacks{});
  if (!result.started) {
    return;  // no PTY available in this environment; nothing to assert
  }

  // 4 MiB is far past any PTY input buffer, so a blocking backend would park
  // here for the child's lifetime.
  const std::string payload(4u << 20, 'x');
  const auto start = std::chrono::steady_clock::now();
  backend->Write(payload);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  backend->Stop();

  Expect(elapsed_ms < 2000,
         "PosixTerminalBackend::Write must not block on a child that stops reading stdin");
}

// The buffered write path must still deliver input: a draining child (`cat`
// echoes stdin) round-trips the payload back through the PTY, exercising
// Write() -> pending buffer -> reader-thread POLLOUT drain -> child.
void TestTerminalBackendBufferedWriteReachesDrainingChild() {
  auto backend = CreateTerminalBackend();

  std::mutex mutex;
  std::condition_variable cv;
  std::string output;
  TerminalBackendCallbacks callbacks;
  callbacks.on_output = [&](std::string_view bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    output.append(bytes);
    cv.notify_all();
  };

  const auto result = backend->Start(
      TerminalStartRequest{.command = "cat", .rows = 24, .columns = 80}, std::move(callbacks));
  if (!result.started) {
    return;
  }

  backend->Write("microide-roundtrip\n");
  bool saw_echo = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    saw_echo = cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return output.find("microide-roundtrip") != std::string::npos;
    });
  }
  backend->Stop();

  Expect(saw_echo,
         "buffered PTY writes must reach a draining child (round-trip through the write queue)");
}

// The shell inherits the editor's SIGPIPE disposition and cannot reset one that
// was ignored at startup, so `yes | head` in the integrated terminal printed a
// "Broken pipe" error where every other terminal shows nothing.
void TestTerminalBackendShellGetsDefaultSigpipe() {
  microide::platform::IgnoreBrokenPipeSignal();
  auto backend = CreateTerminalBackend();

  std::mutex mutex;
  std::condition_variable cv;
  std::string output;
  TerminalBackendCallbacks callbacks;
  callbacks.on_output = [&](std::string_view bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    output.append(bytes);
    cv.notify_all();
  };

  const auto result = backend->Start(
      TerminalStartRequest{.command = "( yes 2>/dev/null; echo yes-exit=$? >&2 ) | head -c 1 "
                                      ">/dev/null; echo probe-done",
                           .rows = 24,
                           .columns = 80},
      std::move(callbacks));
  if (!result.started) {
    return;
  }
  bool saw_marker = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    saw_marker = cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return output.find("probe-done") != std::string::npos;
    });
  }
  backend->Stop();
  Expect(saw_marker, "the SIGPIPE probe command should finish");
  Expect(output.find("yes-exit=141") != std::string::npos,
         "the terminal shell's children must see SIGPIPE at its default, got: " + output);
}

// The `terminal.shell` setting has always been described as a shell COMMAND, and the
// backend treated it as a program path: it exec'd it with fixed arguments, so a
// multi-word value ran the first word with `-i` appended (`ssh host` became
// `ssh -i`, ssh's identity-file flag) and a bare program name never resolved through
// PATH at all. Both halves are pinned here.
void TestTerminalBackendShellIsACommandLineNotAProgramPath() {
  auto backend = CreateTerminalBackend();

  std::mutex mutex;
  std::condition_variable cv;
  std::string output;
  TerminalBackendCallbacks callbacks;
  callbacks.on_output = [&](std::string_view bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    output.append(bytes);
    cv.notify_all();
  };

  // A multi-word shell is exec'd exactly as written — no `-i` appended.
  const auto result = backend->Start(
      TerminalStartRequest{.shell = {"/bin/sh", "-c", "echo argv-shell-ran"},
                           .rows = 24,
                           .columns = 80},
      std::move(callbacks));
  if (!result.started) {
    return;  // no PTY available in this environment
  }
  bool saw_marker = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    saw_marker = cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return output.find("argv-shell-ran") != std::string::npos;
    });
  }
  backend->Stop();
  Expect(saw_marker,
         "a multi-word terminal.shell must be exec'd as written, got: " + output);
}

// TD: `-lc` used to be appended to EVERY shell with a command, including a
// multi-word one. `terminal.shell = "ssh build-host"` plus the `terminal <cmd>`
// search action produced `ssh build-host -lc <cmd>`, which ssh reads as `-l c`:
// it logs in as user "c" and runs the command remotely. That is the `-i` bug --
// the one this field's multi-word form exists to fix -- surviving on the command
// path, and it SUCCEEDS at the wrong thing rather than failing, so nothing
// surfaces it. Pinned on the argv builder so it holds with no PTY available.
void TestTerminalArgvAttachesCommandsWithoutInventingFlags() {
  using microide::platform::BuildTerminalArgv;
  const auto expect = [](const std::vector<std::string>& shell, const std::string& command,
                         const std::vector<std::string>& expected) {
    const std::vector<std::string> actual = BuildTerminalArgv(shell, command);
    std::string shown = "[";
    for (std::size_t i = 0; i < actual.size(); ++i) {
      shown += (i == 0 ? "\"" : ", \"") + actual[i] + '"';
    }
    shown += ']';
    Expect(actual == expected, "BuildTerminalArgv produced " + shown);
  };

  // A bare shell name: a shell by definition, so the shell conventions apply.
  expect({"bash"}, "", {"bash", "-i"});
  expect({"/bin/bash"}, "", {"bash", "-i"});
  expect({"bash"}, "make", {"bash", "-lc", "make"});
  // The regression. A launcher takes the command as a trailing argument; it must
  // never be handed `-lc`, which is a flag of its own to every one of them.
  expect({"ssh", "build-host"}, "make", {"ssh", "build-host", "make"});
  expect({"docker", "exec", "-it", "box"}, "make", {"docker", "exec", "-it", "box", "make"});
  // ...and with no command, still exactly as written.
  expect({"ssh", "build-host"}, "", {"ssh", "build-host"});
  // A multi-word value that IS a shell keeps the `-c` convention, so
  // `terminal.shell = "bash --norc"` still runs commands. `-c`, not `-lc`: the
  // user's own flags decide the rest.
  expect({"bash", "--norc"}, "make", {"bash", "--norc", "-c", "make"});
  expect({"/usr/bin/zsh", "-f"}, "make", {"/usr/bin/zsh", "-f", "-c", "make"});
  // argv[0] is rewritten to the base name only in the bare form; a multi-word
  // value is exec'd exactly as written, path and all.
  expect({"/bin/sh", "-c", "echo hi"}, "", {"/bin/sh", "-c", "echo hi"});
}

void TestTerminalBackendBareShellNameResolvesThroughPath() {
  auto backend = CreateTerminalBackend();

  std::mutex mutex;
  std::condition_variable cv;
  std::string output;
  TerminalBackendCallbacks callbacks;
  callbacks.on_output = [&](std::string_view bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    output.append(bytes);
    cv.notify_all();
  };

  // `sh`, not `/bin/sh`: the old execl() required an absolute path, so a user who
  // set terminal.shell to a bare program name got a terminal that exited instantly
  // with no explanation.
  const auto result = backend->Start(
      TerminalStartRequest{.command = "echo path-resolved-shell",
                           .shell = {"sh"},
                           .rows = 24,
                           .columns = 80},
      std::move(callbacks));
  if (!result.started) {
    return;
  }
  bool saw_marker = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    saw_marker = cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return output.find("path-resolved-shell") != std::string::npos;
    });
  }
  backend->Stop();
  Expect(saw_marker,
         "a bare terminal.shell program name must resolve through PATH, got: " + output);
}

#endif  // defined(__unix__) || defined(__APPLE__)

}  // namespace

void RegisterTerminalBackendTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "TerminalBackend/WriteDoesNotBlockOnStuckChild",
          TestTerminalBackendWriteDoesNotBlockOnStuckChild);
  AddTest(tests, "TerminalBackend/BufferedWriteReachesDrainingChild",
          TestTerminalBackendBufferedWriteReachesDrainingChild);
  AddTest(tests, "TerminalBackend/ShellGetsDefaultSigpipe",
          TestTerminalBackendShellGetsDefaultSigpipe);
  AddTest(tests, "TerminalBackend/ShellIsACommandLineNotAProgramPath",
          TestTerminalBackendShellIsACommandLineNotAProgramPath);
  AddTest(tests, "TerminalBackend/BareShellNameResolvesThroughPath",
          TestTerminalBackendBareShellNameResolvesThroughPath);
  AddTest(tests, "TerminalBackend/ArgvAttachesCommandsWithoutInventingFlags",
          TestTerminalArgvAttachesCommandsWithoutInventingFlags);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
