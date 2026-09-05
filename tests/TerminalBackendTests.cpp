#include "TestSupport.h"

#include "platform/TerminalBackend.h"

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
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
