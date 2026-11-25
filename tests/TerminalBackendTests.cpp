#include "TestSupport.h"

#include "platform/HostPlatform.h"
#include "platform/ShellProcess.h"
#include "platform/TerminalBackend.h"

#include <string>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace microide::tests {
namespace {

using microide::platform::CreateTerminalBackend;
using microide::platform::TerminalBackendCallbacks;
using microide::platform::TerminalStartRequest;

void TestTerminalBackendWriteDoesNotBlockOnStuckChild() {
  auto backend = CreateTerminalBackend();
  const auto result = backend->Start(
      TerminalStartRequest{.command = "sleep 30", .rows = 24, .columns = 80},
      TerminalBackendCallbacks{});
  if (!result.started) {
    return;
  }

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

  const auto result = backend->Start(
      TerminalStartRequest{.shell = {"/bin/sh", "-c", "echo argv-shell-ran"},
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
      return output.find("argv-shell-ran") != std::string::npos;
    });
  }
  backend->Stop();
  Expect(saw_marker,
         "a multi-word terminal.shell must be exec'd as written, got: " + output);
}

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

  expect({"bash"}, "", {"bash", "-i"});
  expect({"/bin/bash"}, "", {"bash", "-i"});
  expect({"bash"}, "make", {"bash", "-lc", "make"});
  expect({"ssh", "build-host"}, "make", {"ssh", "build-host", "make"});
  expect({"docker", "exec", "-it", "box"}, "make", {"docker", "exec", "-it", "box", "make"});
  expect({"ssh", "build-host"}, "", {"ssh", "build-host"});
  expect({"bash", "--norc"}, "make", {"bash", "--norc", "-c", "make"});
  expect({"/usr/bin/zsh", "-f"}, "make", {"/usr/bin/zsh", "-f", "-c", "make"});
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

void TestTerminalBackendReportsOutputItHasNotDelivered() {
  auto backend = CreateTerminalBackend();
  std::mutex mutex;
  std::condition_variable cv;
  std::string output;
  bool entered = false;
  bool released = false;
  TerminalBackendCallbacks callbacks;
  callbacks.on_output = [&](std::string_view bytes) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
    output.append(bytes);
  };
  const auto result = backend->Start(
      TerminalStartRequest{.command = "printf first; sleep 0.3; printf second; sleep 30",
                           .rows = 24,
                           .columns = 80},
      std::move(callbacks));
  if (!result.started) {
    return;
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    Expect(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
           "the first output reaches on_output");
  }
  Expect(backend->HasUnreadOutput(), "output being delivered is not yet delivered");
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  Expect(backend->HasUnreadOutput(), "nor is output still in the pty");
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  cv.notify_all();
  Expect(WaitUntil([&] { return !backend->HasUnreadOutput(); }, std::chrono::seconds(5)),
         "once delivered, nothing is unread (the program is asleep)");
  {
    std::lock_guard<std::mutex> lock(mutex);
    Expect(output.find("second") != std::string::npos,
           "and nothing unread means everything printed was delivered: '" + output + "'");
  }
  backend->Stop();
}

}

void RegisterTerminalBackendTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "TerminalBackend/ReportsOutputItHasNotDelivered",
          TestTerminalBackendReportsOutputItHasNotDelivered);
}

}
